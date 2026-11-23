#include "Actors.hpp"

#include "ActorRegistry.hpp"
#include "AgentManager.hpp"

#include "../desktop/view/window/Window.hpp"
#include "../desktop/view/LayerSurface.hpp"
#include "../output/Monitor.hpp"
#include "../protocols/IdleNotify.hpp"
#include "../protocols/IdleInhibit.hpp"
#include "../protocols/PointerConstraints.hpp"
#include "../protocols/Screencopy.hpp"
#include "../protocols/ShortcutsInhibit.hpp"
#include "../protocols/VirtualKeyboard.hpp"
#include "../protocols/VirtualPointer.hpp"
#include "../protocols/XDGOutput.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/Output.hpp"
#include "../workspace/HLWorkspace.hpp"

#include <wayland-server-protocol.h>

static std::optional<Actors::CPendingOutputGlobal::SScope> s_pendingOutput;

Actors::CPendingOutputGlobal::CPendingOutputGlobal(ActorID actor) : m_previous(s_pendingOutput) {
    s_pendingOutput = SScope{.actor = actor};
}

Actors::CPendingOutputGlobal::CPendingOutputGlobal(const wl_client* onlyClient) : m_previous(s_pendingOutput) {
    s_pendingOutput = SScope{.client = onlyClient};
}

Actors::CPendingOutputGlobal::~CPendingOutputGlobal() {
    s_pendingOutput = m_previous;
}

std::optional<Actors::ActorID> Actors::ownerOf(const wl_client* client) {
    if (!g_pActorRegistry)
        return HUMAN_ACTOR_ID;

    return g_pActorRegistry->ownerOf(client);
}

std::optional<Actors::ActorID> Actors::controllerOf(const wl_client* client) {
    if (!g_pActorRegistry)
        return HUMAN_ACTOR_ID;

    return g_pActorRegistry->controllerOf(client);
}

std::optional<Actors::ActorID> Actors::controllerOf(const SP<CWLSurfaceResource>& surface) {
    if (!surface)
        return std::nullopt;

    return controllerOf(surface->client());
}

Actors::ActorID Actors::controllerOf(const PHLWINDOW& window) {
    if (!window)
        return HUMAN_ACTOR_ID;

    const auto SURFACE = window->wlSurface() ? window->wlSurface()->resource() : nullptr;
    if (SURFACE) {
        if (const auto CONTROLLER = controllerOf(SURFACE->client()); CONTROLLER)
            return *CONTROLLER;
    }

    return window->owner();
}

Actors::ActorID Actors::actorOf(const PHLMONITOR& monitor) {
    return monitor ? monitor->m_actor : HUMAN_ACTOR_ID;
}

Actors::ActorID Actors::actorOf(const PHLWORKSPACE& workspace) {
    return workspace ? workspace->actor() : HUMAN_ACTOR_ID;
}

bool Actors::isAgent(ActorID id) {
    return g_pActorRegistry && g_pActorRegistry->isAgent(id);
}

bool Actors::isAgentMonitor(const PHLMONITOR& monitor) {
    return monitor && isAgent(monitor->m_actor);
}

bool Actors::humanControls(const wl_client* client) {
    return controllerOf(client) == HUMAN_ACTOR_ID;
}

bool Actors::humanControls(const SP<CWLSurfaceResource>& surface) {
    return surface && humanControls(surface->client());
}

bool Actors::humanControls(const PHLWINDOW& window) {
    return controllerOf(window) == HUMAN_ACTOR_ID;
}

bool Actors::isBorrowed(const PHLWINDOW& window) {
    return window && isAgent(window->owner()) && humanControls(window);
}

std::vector<std::string> Actors::borrowTags(ActorID owner) {
    std::vector<std::string> tags = {"borrowed"};
    if (const auto ACTOR = g_pActorRegistry ? g_pActorRegistry->actor(owner) : nullptr; ACTOR)
        tags.emplace_back(ACTOR->name);

    return tags;
}

bool Actors::humanSees(const PHLMONITOR& monitor) {
    return !isAgentMonitor(monitor);
}

bool Actors::humanSees(const PHLWORKSPACE& workspace) {
    return !isAgent(actorOf(workspace));
}

// The workspace covers a borrowed window whose surface is already gone, as on destroy.
bool Actors::humanSees(const PHLWINDOW& window) {
    return window && (humanControls(window) || (window->m_workspace && humanSees(window->m_workspace)));
}

bool Actors::humanSees(const PHLLS& layer) {
    return layer && humanSees(layer->m_monitor.lock());
}

// Privileged globals an agent still gets. Virtual devices drive the agent's own seat. With
// screencopy and xdg_output an agent sees its own screen: its wl_outputs are only its own
// monitors, and mayCapture refuses the human's monitors behind a borrow proxy.
static bool isAgentPrivilegedGlobal(const wl_global* global) {
    const auto IS = [global](const auto& protocol) { return protocol && global == protocol->getGlobal(); };

    return IS(PROTO::virtualKeyboard) || IS(PROTO::virtualPointer) || IS(PROTO::screencopy) || IS(PROTO::xdgOutput);
}

static bool isHiddenFromAgents(const wl_global* global) {
    const auto IS = [global](const auto& protocol) { return protocol && global == protocol->getGlobal(); };

    // ext_idle_notifier reports human activity. Idle inhibitors would keep the human's
    // screen on. Shortcut inhibitors and pointer constraints act on the human's devices.
    return IS(PROTO::idle) || IS(PROTO::idleInhibit) || IS(PROTO::shortcutsInhibit) || IS(PROTO::constraints);
}

bool Actors::globalVisibleTo(const wl_client* client, const wl_global* global, bool privileged) {
    const auto& registry = g_pActorRegistry;
    if (!registry)
        return true;

    const auto OWNER = registry->ownerOf(client);
    if (!OWNER)
        return false;

    // A wl_output global belongs to exactly one actor: the actor of its monitor.
    const auto MONITOR_OUTPUT_VISIBLE = [&](const SP<CWLOutputProtocol>& output) -> std::optional<bool> {
        if (!output || output->getGlobal() != global)
            return std::nullopt;

        const auto MONITOR = output->m_monitor.lock();
        return MONITOR ? MONITOR->m_actor == *OWNER : *OWNER == HUMAN_ACTOR_ID;
    };

    for (const auto& [name, output] : PROTO::outputs) {
        if (const auto VISIBLE = MONITOR_OUTPUT_VISIBLE(output); VISIBLE)
            return *VISIBLE;
    }

    // A borrow proxy output is visible only to the borrowed client.
    if (g_pAgentManager) {
        if (const auto PROXYCLIENT = g_pAgentManager->proxyOutputClient(global); PROXYCLIENT.has_value())
            return *PROXYCLIENT == client;
    }

    // Retired outputs still need the right audience for their global_remove event. Proxies
    // are retired here too, so this check runs after the proxy check.
    for (const auto& output : PROTO::defunctOutputs) {
        if (const auto VISIBLE = MONITOR_OUTPUT_VISIBLE(output); VISIBLE)
            return *VISIBLE;
    }

    // A wl_output global that is not registered yet is being created right now.
    if (wl_global_get_interface(global) == &wl_output_interface) {
        if (!s_pendingOutput)
            return *OWNER == HUMAN_ACTOR_ID;

        if (s_pendingOutput->client)
            return s_pendingOutput->client == client;

        return s_pendingOutput->actor == *OWNER;
    }

    if (!registry->isAgent(*OWNER))
        return true;

    if (isHiddenFromAgents(global))
        return false;

    return !privileged || isAgentPrivilegedGlobal(global);
}

bool Actors::mayCapture(const wl_client* client, const PHLMONITOR& monitor) {
    if (!monitor)
        return false;

    const auto OWNER = ownerOf(client);
    if (!OWNER)
        return false;

    return !isAgent(*OWNER) || monitor->m_actor == *OWNER;
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
