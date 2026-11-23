#include "Actors.hpp"

#include "ActorRegistry.hpp"
#include "AgentManager.hpp"
#include "AgentSeat.hpp"

#include "../debug/log/Logger.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../managers/SeatManager.hpp"
#include "../output/Monitor.hpp"
#include "../protocols/IdleInhibit.hpp"
#include "../protocols/IdleNotify.hpp"
#include "../protocols/PointerConstraints.hpp"
#include "../protocols/Screencopy.hpp"
#include "../protocols/ShortcutsInhibit.hpp"
#include "../protocols/VirtualKeyboard.hpp"
#include "../protocols/VirtualPointer.hpp"
#include "../protocols/WaylandProtocol.hpp"
#include "../protocols/XDGOutput.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/Output.hpp"
#include "../protocols/types/DataDevice.hpp"

#include <wayland-server-protocol.h>

/*
    The rules that keep actors apart. Every decision that depends on who owns or controls
    something is made here, with its log message, so callers stay one line long.
*/

using namespace Actors;

// ------------------------------------------------------------------ focus and input

// Focus reasons that come from a window's lifecycle. Reasons that come from human input
// (keybinds, dispatchers, clicks, follow mouse) never move an agent's focus.
static bool agentFocusReasonAllowed(Desktop::eFocusReason reason) {
    switch (reason) {
        case Desktop::FOCUS_REASON_UNKNOWN:
        case Desktop::FOCUS_REASON_NEW_WINDOW:
        case Desktop::FOCUS_REASON_UNMAP_WINDOW_TILING:
        case Desktop::FOCUS_REASON_UNMAP_WINDOW_FLOATING:
        case Desktop::FOCUS_REASON_UNMAP_GROUPED_WINDOW:
        case Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE:
        case Desktop::FOCUS_REASON_OTHER: return true;
        default: return false;
    }
}

bool Actors::refuseHumanFocus(const PHLWINDOW& window, Desktop::eFocusReason reason) {
    if (!window || humanControls(window) || agentFocusReasonAllowed(reason))
        return false;

    LOG(Log::DEBUG, "Refusing human focus request for {}: the human does not control it", window);
    return true;
}

bool Actors::routeWindowFocus(const PHLWINDOW& window, Desktop::eFocusReason reason, const SP<CWLSurfaceResource>& surface) {
    if (!window || humanControls(window))
        return false;

    if (refuseHumanFocus(window, reason))
        return true;

    if (const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(controllerOf(window)) : nullptr; SEAT)
        SEAT->focusWindow(window, surface);

    return true;
}

bool Actors::routeSurfaceFocus(const SP<CWLSurfaceResource>& surface) {
    if (!surface || humanControls(surface))
        return false;

    if (const auto CONTROLLER = controllerOf(surface); CONTROLLER && g_pAgentManager) {
        if (const auto SEAT = g_pAgentManager->seat(*CONTROLLER); SEAT)
            SEAT->focusSurface(surface);
    }

    return true;
}

void Actors::focusAgentWindow(ActorID agent, const PHLWINDOW& window) {
    if (const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(agent) : nullptr; SEAT)
        SEAT->focusWindow(window);
}

bool Actors::isFocused(const PHLWINDOW& window) {
    if (!window)
        return false;

    if (humanControls(window))
        return Desktop::focusState()->window() == window;

    const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(controllerOf(window)) : nullptr;
    return SEAT && SEAT->focusedWindow() == window;
}

std::optional<bool> Actors::agentWindowActive(const PHLWINDOW& window) {
    if (!window || humanControls(window))
        return std::nullopt;

    const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(controllerOf(window)) : nullptr;
    return SEAT && SEAT->isWindowActive(window);
}

static CAgentSeat* agentSeatOf(const wl_client* client) {
    if (!g_pAgentManager || humanControls(client))
        return nullptr;

    const auto CONTROLLER = controllerOf(client);
    return CONTROLLER ? g_pAgentManager->seat(*CONTROLLER) : nullptr;
}

SP<IKeyboard> Actors::keyboardFor(const wl_client* client) {
    // Until an agent's virtual keyboard appears, its clients get the human keymap.
    if (const auto SEAT = agentSeatOf(client); SEAT) {
        if (auto keyboard = SEAT->keyboard())
            return keyboard;
    }

    return g_pSeatManager->m_keyboard.lock();
}

SP<CWLSurfaceResource> Actors::keyboardFocusFor(const wl_client* client) {
    if (humanControls(client))
        return g_pSeatManager->m_state.keyboardFocus.lock();

    const auto SEAT = agentSeatOf(client);
    return SEAT ? SEAT->keyboardFocus() : nullptr;
}

SP<CWLSurfaceResource> Actors::pointerFocusFor(const wl_client* client) {
    if (humanControls(client))
        return g_pSeatManager->m_state.pointerFocus.lock();

    const auto SEAT = agentSeatOf(client);
    return SEAT ? SEAT->pointerFocus() : nullptr;
}

// A device belongs to its client's owner. A borrowed client's virtual devices still drive
// its agent, so the human seat never gets input from an agent.
bool Actors::adoptVirtualKeyboard(const wl_client* client, const SP<IKeyboard>& keyboard) {
    const auto OWNER = ownerOf(client);
    if (!OWNER || !isAgent(*OWNER))
        return false;

    if (const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(*OWNER) : nullptr; SEAT)
        SEAT->addKeyboard(keyboard);
    else
        LOG(Log::ERR, "Virtual keyboard for {} without an agent seat", nameOf(*OWNER));

    return true;
}

bool Actors::adoptVirtualPointer(const wl_client* client, const SP<IPointer>& pointer) {
    const auto OWNER = ownerOf(client);
    if (!OWNER || !isAgent(*OWNER))
        return false;

    if (const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(*OWNER) : nullptr; SEAT)
        SEAT->addPointer(pointer);
    else
        LOG(Log::ERR, "Virtual pointer for {} without an agent seat", nameOf(*OWNER));

    return true;
}

bool Actors::routeSelection(const wl_client* client, const SP<IDataSource>& source, bool primary) {
    if (humanControls(client))
        return false;

    if (source)
        source->markUsed();

    if (const auto SEAT = agentSeatOf(client); SEAT)
        SEAT->setSelection(source, primary);
    else if (source)
        source->cancelled();

    return true;
}

bool Actors::routePopupGrab(const wl_client* client, const SP<CXDGPopupResource>& popup) {
    if (!popup || humanControls(client))
        return false;

    if (const auto SEAT = agentSeatOf(client); SEAT)
        SEAT->addPopupGrab(popup);
    else
        popup->done();

    return true;
}

// ------------------------------------------------------------------ protocol rules

// Privileged globals an agent still gets. Virtual devices drive the agent's own seat. With
// screencopy and xdg_output an agent sees its own screen: its wl_outputs are only its own
// monitors, and mayCapture refuses the human's monitors behind a borrow proxy.
static bool isAgentPrivilegedGlobal(const wl_global* global) {
    const auto IS = [global](const auto& protocol) { return protocol && global == protocol->getGlobal(); };

    return IS(PROTO::virtualKeyboard) || IS(PROTO::virtualPointer) || IS(PROTO::screencopy) || IS(PROTO::xdgOutput);
}

// ext_idle_notifier reports human activity. Idle inhibitors would keep the human's screen on.
// Shortcut inhibitors and pointer constraints act on the human's devices.
static bool isHiddenFromAgents(const wl_global* global) {
    const auto IS = [global](const auto& protocol) { return protocol && global == protocol->getGlobal(); };

    return IS(PROTO::idle) || IS(PROTO::idleInhibit) || IS(PROTO::shortcutsInhibit) || IS(PROTO::constraints);
}

bool Actors::globalVisibleTo(const wl_client* client, const wl_global* global, bool privileged) {
    if (!g_pActorRegistry)
        return true;

    const auto OWNER = g_pActorRegistry->ownerOf(client);
    if (!OWNER)
        return false;

    // A wl_output global has an audience (outputAudience, or the one client of a borrow
    // proxy), which the compositor checked already. Without one, it is the human's.
    if (wl_global_get_interface(global) == &wl_output_interface) {
        const auto PROTOCOL = IWaylandProtocol::fromGlobal(global);
        return (PROTOCOL && PROTOCOL->hasClientFilter()) || *OWNER == HUMAN_ACTOR_ID;
    }

    if (!isAgent(*OWNER))
        return true;

    if (isHiddenFromAgents(global))
        return false;

    return !privileged || isAgentPrivilegedGlobal(global);
}

std::function<bool(const wl_client*)> Actors::outputAudience(const PHLMONITOR& monitor) {
    // A monitor's actor never changes, so the audience is fixed even after the monitor is gone
    // and the retired global still needs it for global_remove.
    return [ACTOR = actorOf(monitor)](const wl_client* client) { return ownerOf(client) == ACTOR; };
}

bool Actors::mayCapture(const wl_client* client, const PHLMONITOR& monitor) {
    if (!monitor)
        return false;

    const auto OWNER = ownerOf(client);
    if (!OWNER)
        return false;

    if (isAgent(*OWNER) && monitor->m_actor != *OWNER) {
        LOG(Log::WARN, "{} may not capture monitor {}", nameOf(*OWNER), monitor->m_name);
        return false;
    }

    return true;
}

bool Actors::mayActivate(const wl_client* issuer, const PHLWINDOW& window) {
    const auto ISSUER = issuer ? controllerOf(issuer) : std::optional<ActorID>{HUMAN_ACTOR_ID};
    if (ISSUER && *ISSUER == controllerOf(window))
        return true;

    LOG(Log::DEBUG, "Ignoring an activation token for {}: it crosses actors", window);
    return false;
}

std::vector<SP<CWLOutputResource>> Actors::outputResourcesFor(const PHLMONITOR& monitor, wl_client* client) {
    std::vector<SP<CWLOutputResource>> resources;
    if (!monitor)
        return resources;

    if (PROTO::outputs.contains(monitor->m_name))
        resources = PROTO::outputs.at(monitor->m_name)->outputResourcesFrom(client);

    if (g_pAgentManager) {
        for (auto& proxy : g_pAgentManager->proxyOutputResources(monitor, client))
            resources.emplace_back(std::move(proxy));
    }

    return resources;
}
