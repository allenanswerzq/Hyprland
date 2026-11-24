#include "AgentSeat.hpp"

#include "Actors.hpp"
#include "ActorRegistry.hpp"
#include "AgentManager.hpp"

#include "../config/ConfigValue.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/state/ViewState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../desktop/view/window/WindowGroupMembership.hpp"
#include "../desktop/view/window/WindowPresentation.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../helpers/time/Time.hpp"
#include "../ipc/s2/S2.hpp"
#include "../layout/LayoutManager.hpp"
#include "../layout/target/WindowTarget.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/SeatManager.hpp"
#include "../output/Monitor.hpp"
#include "../protocols/PrimarySelection.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/core/Seat.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>

using namespace Actors;

template <typename F>
static void forEachKeyboard(const wl_client* client, F&& fn) {
    if (!client || !PROTO::seat)
        return;

    for (const auto& seat : PROTO::seat->seatResourcesForClient(client)) {
        for (const auto& keyboard : seat->m_keyboards) {
            if (keyboard)
                fn(keyboard.lock());
        }
    }
}

template <typename F>
static void forEachPointer(const wl_client* client, F&& fn) {
    if (!client || !PROTO::seat)
        return;

    for (const auto& seat : PROTO::seat->seatResourcesForClient(client)) {
        for (const auto& pointer : seat->m_pointers) {
            if (pointer)
                fn(pointer.lock());
        }
    }
}

CAgentSeat::CAgentSeat(ActorID actor) : m_actor(actor), m_popupGrab(makeShared<CSeatGrab>()) {
    m_popupGrab->m_keyboard = true;
    m_popupGrab->m_pointer  = true;
}

CAgentSeat::~CAgentSeat() = default;

ActorID CAgentSeat::actor() const {
    return m_actor;
}

void CAgentSeat::addKeyboard(SP<IKeyboard> keyboard) {
    if (!keyboard)
        return;

    static auto PREPEATRATE  = CConfigValue<Config::INTEGER>("input:repeat_rate");
    static auto PREPEATDELAY = CConfigValue<Config::INTEGER>("input:repeat_delay");

    keyboard->m_repeatRate  = std::max(0, sc<int>(*PREPEATRATE));
    keyboard->m_repeatDelay = std::max(0, sc<int>(*PREPEATDELAY));

    // Agent keyboards skip the input manager, which names the human's devices. The name shows
    // in the agent's activelayout events.
    if (keyboard->m_hlName.empty())
        keyboard->m_hlName = std::format("{}-{}", nameOf(m_actor), deviceNameToInternalString(keyboard->m_deviceName));

    // A virtual keyboard uploads its own keymap before its first key. Until then it uses the
    // default xkb keymap, so the agent's clients always have one.
    if (!keyboard->m_xkbKeymap)
        keyboard->setKeymap({});

    auto&      entry = m_keyboards.emplace_back();
    IKeyboard* raw   = keyboard.get();

    entry.device    = keyboard;
    entry.key       = keyboard->m_keyboardEvents.key.listen([this, raw](const IKeyboard::SKeyEvent& event) { onKey(raw->m_self.lock(), event); });
    entry.modifiers = keyboard->m_keyboardEvents.modifiers.listen([this, raw](const IKeyboard::SModifiersEvent& event) { onModifiers(raw->m_self.lock(), event); });
    entry.keymap    = keyboard->m_keyboardEvents.keymap.listen([this, raw] { onKeymap(raw->m_self.lock()); });
    entry.destroy   = keyboard->m_events.destroy.listen([this, raw] {
        // A keyboard sends destroy twice: when its client goes away, and again from ~IKeyboard.
        // m_self is empty during the second one, so only the first one runs. Holding KEEPALIVE
        // also delays ~IKeyboard until the erase below has finished; otherwise the erase frees
        // the keyboard, its destructor runs this code again, and the entry is erased twice.
        const auto KEEPALIVE = raw->m_self.lock();
        if (!KEEPALIVE)
            return;

        if (m_activeKeyboard.get() == raw) {
            m_activeKeyboard.reset();

            for (const auto& other : m_keyboards | std::views::reverse) {
                if (other.device.get() != raw) {
                    useKeyboard(other.device);
                    break;
                }
            }
        }

        std::erase_if(m_keyboards, [raw](const auto& other) { return other.device.get() == raw; });
    });

    if (!m_activeKeyboard)
        useKeyboard(keyboard);

    LOG(Log::DEBUG, "Agent {} got a virtual keyboard", m_actor);
}

void CAgentSeat::addPointer(SP<IPointer> pointer) {
    if (!pointer)
        return;

    auto&     entry = m_pointers.emplace_back();
    IPointer* raw   = pointer.get();

    entry.device         = pointer;
    entry.motion         = pointer->m_pointerEvents.motion.listen([this](const IPointer::SMotionEvent& event) { onMotion(event.timeMs, event.delta); });
    entry.motionAbsolute = pointer->m_pointerEvents.motionAbsolute.listen([this](const IPointer::SMotionAbsoluteEvent& event) { onMotionAbsolute(event.timeMs, event.absolute); });
    entry.button         = pointer->m_pointerEvents.button.listen([this](const IPointer::SButtonEvent& event) { onButton(event); });
    entry.axis           = pointer->m_pointerEvents.axis.listen([this](const IPointer::SAxisEvent& event) { onAxis(event); });
    entry.frame          = pointer->m_pointerEvents.frame.listen([this] { onFrame(); });
    entry.destroy        = pointer->m_events.destroy.listen([this, raw] {
        // Same as for keyboards: keep the pointer alive until the erase has finished.
        const auto KEEPALIVE = raw->m_self.lock();
        if (!KEEPALIVE)
            return;

        std::erase_if(m_pointers, [raw](const auto& other) { return other.device.get() == raw; });
    });

    onMonitorChanged();

    LOG(Log::DEBUG, "Agent {} got a virtual pointer", m_actor);
}

SP<IKeyboard> CAgentSeat::keyboard() const {
    return m_activeKeyboard.lock();
}

void CAgentSeat::useKeyboard(SP<IKeyboard> keyboard) {
    if (!keyboard || m_activeKeyboard == keyboard)
        return;

    m_activeKeyboard = keyboard;
    sendKeymap(keyboard);
}

// Sends the keymap and repeat info to every wl_keyboard of the clients this agent controls.
void CAgentSeat::sendKeymap(const SP<IKeyboard>& keyboard) const {
    if (!keyboard || !g_pActorRegistry)
        return;

    // A client the agent owns but the human borrowed follows the human's keyboard instead.
    for (const auto CLIENT : g_pActorRegistry->clientsOwnedBy(m_actor)) {
        if (controllerOf(CLIENT) != m_actor)
            continue;

        forEachKeyboard(CLIENT, [&](const SP<CWLKeyboardResource>& k) {
            k->sendKeymap(keyboard);
            k->repeatInfo(keyboard->m_repeatRate, keyboard->m_repeatDelay);
        });
    }
}

void CAgentSeat::onKey(SP<IKeyboard> keyboard, const IKeyboard::SKeyEvent& event) {
    if (!keyboard)
        return;

    useKeyboard(keyboard);

    const auto FOCUS = m_keyboardFocus.lock();
    if (!FOCUS)
        return;

    forEachKeyboard(FOCUS->client(), [&](const SP<CWLKeyboardResource>& k) { k->sendKey(event.timeMs, event.keycode, event.state); });
}

void CAgentSeat::onModifiers(SP<IKeyboard> keyboard, const IKeyboard::SModifiersEvent& event) {
    if (!keyboard)
        return;

    useKeyboard(keyboard);

    const auto FOCUS = m_keyboardFocus.lock();
    if (!FOCUS)
        return;

    forEachKeyboard(FOCUS->client(), [&](const SP<CWLKeyboardResource>& k) { k->sendMods(event.depressed, event.latched, event.locked, event.group); });
}

void CAgentSeat::onKeymap(SP<IKeyboard> keyboard) {
    if (!keyboard)
        return;

    // The keyboard that just uploaded a keymap is the one the agent is about to type with.
    m_activeKeyboard = keyboard;
    sendKeymap(keyboard);

    IPC::Socket2::sock()->postEvent({.event = "activelayout", .data = std::format("{},{}", keyboard->m_hlName, keyboard->getActiveLayout()), .subject = m_actor});
}

CBox CAgentSeat::monitorBox() const {
    const auto MONITOR = g_pAgentManager ? g_pAgentManager->monitorFor(m_actor) : nullptr;
    return MONITOR ? MONITOR->logicalBox() : CBox{};
}

void CAgentSeat::clampPointer() {
    const auto BOX = monitorBox();
    if (BOX.empty())
        return;

    m_pointerPosition.x = std::clamp(m_pointerPosition.x, BOX.x, BOX.x + BOX.w - 1);
    m_pointerPosition.y = std::clamp(m_pointerPosition.y, BOX.y, BOX.y + BOX.h - 1);
}

void CAgentSeat::onMonitorChanged() {
    const auto BOX = monitorBox();
    if (BOX.empty())
        return;

    if (!m_pointerPlaced) {
        m_pointerPosition = BOX.middle();
        m_pointerPlaced   = true;
    }

    clampPointer();
}

void CAgentSeat::onMotion(uint32_t timeMs, const Vector2D& delta) {
    onMonitorChanged();
    m_pointerPosition = m_pointerPosition + delta;
    clampPointer();
    updatePointerFocus(timeMs);
}

void CAgentSeat::onMotionAbsolute(uint32_t timeMs, const Vector2D& absolute) {
    const auto BOX = monitorBox();
    if (BOX.empty())
        return;

    m_pointerPosition = Vector2D{BOX.x + absolute.x * BOX.w, BOX.y + absolute.y * BOX.h};
    m_pointerPlaced   = true;
    clampPointer();
    updatePointerFocus(timeMs);
}

void CAgentSeat::warpPointer(const Vector2D& local) {
    const auto BOX = monitorBox();
    if (BOX.empty())
        return;

    m_pointerPosition = BOX.pos() + local;
    m_pointerPlaced   = true;
    clampPointer();
    updatePointerFocus(Time::millis(Time::steadyNow()));
}

bool CAgentSeat::popupGrabActive() {
    std::erase_if(m_grabbedPopups, [](const auto& popup) { return !popup; });

    if (m_grabbedPopups.empty()) {
        m_popupGrab->clear();
        return false;
    }

    return true;
}

void CAgentSeat::updatePointerFocus(uint32_t timeMs) {
    // While a button is held, the surface that got the press keeps the pointer. This is the
    // implicit grab every Wayland seat has.
    if (!m_pressedButtons.empty()) {
        if (const auto FOCUS = m_pointerFocus.lock(); FOCUS) {
            const auto LOCAL = m_pointerPosition - m_pointerFocusOrigin;
            forEachPointer(FOCUS->client(), [&](const SP<CWLPointerResource>& p) { p->sendMotion(timeMs, LOCAL); });
            return;
        }
    }

    SP<CWLSurfaceResource> surface;
    PHLWINDOW              window;
    Vector2D               local;

    if (const auto WORKSPACE = g_pAgentManager ? g_pAgentManager->workspaceFor(m_actor) : nullptr; WORKSPACE) {
        const auto HIT = Desktop::viewState()->hitTest();
        window         = HIT.windowAtWorkspace(m_pointerPosition, WORKSPACE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

        if (window && controllerOf(window) == m_actor)
            surface = HIT.windowSurfaceAt(m_pointerPosition, window, local);
        else
            window.reset();
    }

    // A popup grab keeps the pointer on the grabbing client's surfaces.
    if (surface && popupGrabActive() && !m_popupGrab->accepts(surface)) {
        surface.reset();
        window.reset();
    }

    if (surface && controllerOf(surface) != m_actor) {
        surface.reset();
        window.reset();
    }

    m_pointerWindow = window;
    setPointerFocus(surface, local);

    if (surface)
        forEachPointer(surface->client(), [&](const SP<CWLPointerResource>& p) { p->sendMotion(timeMs, local); });
}

void CAgentSeat::setPointerFocus(SP<CWLSurfaceResource> surface, const Vector2D& local) {
    const auto OLD = m_pointerFocus.lock();

    if (surface)
        m_pointerFocusOrigin = m_pointerPosition - local;

    if (OLD == surface)
        return;

    m_pointerFocusDestroy.reset();
    m_pointerFocus.reset();

    if (OLD) {
        forEachPointer(OLD->client(), [](const SP<CWLPointerResource>& p) {
            p->sendLeave();
            p->sendFrame();
        });
    }

    m_pressedButtons.clear();

    if (!surface)
        return;

    m_pointerFocus        = surface;
    m_pointerFocusDestroy = surface->m_events.destroy.listen([this] {
        m_pointerFocusDestroy.reset();
        m_pointerFocus.reset();
        m_pointerWindow.reset();
        m_pressedButtons.clear();

        // What is under the pointer now gets it, as the human's pointer does, so a menu that
        // opens under a still pointer after another one closed gets the pointer too. Later:
        // the surface tree is still changing while a surface is destroyed.
        g_pEventLoopManager->doLater([actor = m_actor] {
            if (const auto SEAT = g_pAgentManager ? g_pAgentManager->seat(actor) : nullptr; SEAT && !SEAT->m_pointerFocus)
                SEAT->updatePointerFocus(Time::millis(Time::steadyNow()));
        });
    });

    forEachPointer(surface->client(), [&](const SP<CWLPointerResource>& p) {
        p->sendEnter(surface, local);
        p->sendFrame();
    });

    if (!OLD || OLD->client() != surface->client())
        sendSelection(surface->client(), true);
}

void CAgentSeat::onButton(const IPointer::SButtonEvent& event) {
    onMonitorChanged();

    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // A click outside a popup grab dismisses the popups. The click then goes to whatever
        // is under the pointer.
        if (popupGrabActive()) {
            const auto FOCUS = m_pointerFocus.lock();
            if (!FOCUS || !m_popupGrab->accepts(FOCUS)) {
                endPopupGrab();
                updatePointerFocus(event.timeMs);
            }
        }

        if (m_pressedButtons.empty()) {
            const auto WINDOW = m_pointerWindow.lock();
            if (WINDOW && WINDOW != m_focusedWindow.lock())
                focusWindow(WINDOW);
        }

        if (std::ranges::find(m_pressedButtons, event.button) == m_pressedButtons.end())
            m_pressedButtons.emplace_back(event.button);
    } else
        std::erase(m_pressedButtons, event.button);

    const auto FOCUS = m_pointerFocus.lock();
    if (!FOCUS)
        return;

    forEachPointer(FOCUS->client(), [&](const SP<CWLPointerResource>& p) { p->sendButton(event.timeMs, event.button, event.state); });

    // The implicit grab ends with the last button. The pointer may now be over another surface.
    if (m_pressedButtons.empty() && event.state == WL_POINTER_BUTTON_STATE_RELEASED)
        updatePointerFocus(event.timeMs);
}

void CAgentSeat::onAxis(const IPointer::SAxisEvent& event) {
    const auto FOCUS = m_pointerFocus.lock();
    if (!FOCUS)
        return;

    const int32_t VALUE120 = event.deltaDiscrete;
    const int32_t DISCRETE = VALUE120 == 0 ? 0 : (std::abs(VALUE120) < 120 ? (VALUE120 > 0 ? 1 : -1) : VALUE120 / 120);

    forEachPointer(FOCUS->client(), [&](const SP<CWLPointerResource>& p) {
        p->sendAxis(event.timeMs, event.axis, event.delta);
        p->sendAxisSource(event.source);
        p->sendAxisRelativeDirection(event.axis, event.relativeDirection);

        if (event.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
            if (p->version() >= 8)
                p->sendAxisValue120(event.axis, VALUE120);
            else
                p->sendAxisDiscrete(event.axis, DISCRETE);
        } else if (event.delta == 0)
            p->sendAxisStop(event.timeMs, event.axis);
    });
}

void CAgentSeat::onFrame() {
    const auto FOCUS = m_pointerFocus.lock();
    if (!FOCUS)
        return;

    forEachPointer(FOCUS->client(), [](const SP<CWLPointerResource>& p) { p->sendFrame(); });
}

void CAgentSeat::focusWindow(PHLWINDOW window, SP<CWLSurfaceResource> surface) {
    if (window && (!Desktop::View::validMapped(window) || controllerOf(window) != m_actor))
        return;

    if (window && window->m_ruleApplicator->noFocus().valueOrDefault())
        return;

    const auto PREVIOUS       = m_focusedWindow.lock();
    const auto TARGET_SURFACE = window ? (surface ? surface : window->wlSurface()->resource()) : nullptr;

    if (PREVIOUS == window && m_keyboardFocus.lock() == TARGET_SURFACE)
        return;

    m_focusedWindow = window;

    if (PREVIOUS && PREVIOUS != window && PREVIOUS->mapped()) {
        PREVIOUS->backend().setActive(false);
        PREVIOUS->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_FOCUS);
        PREVIOUS->presentation().refreshValues();
    }

    if (!window) {
        setKeyboardSurface(nullptr);

        if (PREVIOUS) {
            IPC::Socket2::sock()->postEvent({.event = "activewindow", .data = ",", .subject = m_actor});
            IPC::Socket2::sock()->postEvent({.event = "activewindowv2", .data = "", .subject = m_actor});
        }

        return;
    }

    g_layoutManager->bringTargetToTop(window->windowTarget());

    window->backend().setMinimized(false);
    if (!window->backend().traits().overrideRedirect)
        window->backend().restackToTop();
    window->backend().setActive(true);

    if (window->m_workspace)
        window->m_workspace->rememberFocusedWindow(window);

    setKeyboardSurface(TARGET_SURFACE);

    window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_FOCUS);
    window->presentation().onFocusAnimUpdate();
    window->presentation().refreshValues();

    if (window->m_hints & Desktop::View::WINDOW_HINT_URGENT)
        window->m_hints &= ~Desktop::View::WINDOW_HINT_URGENT;

    if (window->grouping().group())
        window->deactivateGroupMembers();

    // The agent's focus, so its socket2 hears it whoever the window belongs to.
    IPC::Socket2::sock()->postEvent({.event = "activewindow", .data = std::format("{},{}", window->metadata().appID(), window->metadata().title()), .subject = m_actor});
    IPC::Socket2::sock()->postEvent({.event = "activewindowv2", .data = std::format("{:x}", rc<uintptr_t>(window.get())), .subject = m_actor});
}

void CAgentSeat::focusSurface(SP<CWLSurfaceResource> surface) {
    if (surface && controllerOf(surface) != m_actor)
        return;

    setKeyboardSurface(surface);
}

void CAgentSeat::setKeyboardSurface(SP<CWLSurfaceResource> surface) {
    const auto OLD = m_keyboardFocus.lock();
    if (OLD == surface)
        return;

    m_keyboardFocusDestroy.reset();
    m_keyboardFocus.reset();

    if (OLD)
        forEachKeyboard(OLD->client(), [](const SP<CWLKeyboardResource>& k) { k->sendLeave(); });

    if (!surface)
        return;

    m_keyboardFocus        = surface;
    m_keyboardFocusDestroy = surface->m_events.destroy.listen([this, raw = surface.get()] {
        m_keyboardFocusDestroy.reset();
        m_keyboardFocus.reset();

        // A popup went away: give the keyboard back to its window.
        const auto WINDOW = m_focusedWindow.lock();
        if (Desktop::View::validMapped(WINDOW) && WINDOW->wlSurface()->resource() && WINDOW->wlSurface()->resource().get() != raw)
            setKeyboardSurface(WINDOW->wlSurface()->resource());
    });

    const auto KEYBOARD = keyboard();

    wl_array   keys;
    wl_array_init(&keys);

    forEachKeyboard(surface->client(), [&](const SP<CWLKeyboardResource>& k) {
        k->sendEnter(surface, &keys);
        if (KEYBOARD)
            k->sendMods(KEYBOARD->m_modifiersState.depressed, KEYBOARD->m_modifiersState.latched, KEYBOARD->m_modifiersState.locked, KEYBOARD->m_modifiersState.group);
    });

    wl_array_release(&keys);

    if (!OLD || OLD->client() != surface->client())
        sendSelection(surface->client(), false);
}

PHLWINDOW CAgentSeat::focusedWindow() const {
    return m_focusedWindow.lock();
}

SP<CWLSurfaceResource> CAgentSeat::keyboardFocus() const {
    return m_keyboardFocus.lock();
}

SP<CWLSurfaceResource> CAgentSeat::pointerFocus() const {
    return m_pointerFocus.lock();
}

bool CAgentSeat::isWindowActive(const PHLWINDOW& window) const {
    if (!window || !window->mapped())
        return false;

    const auto SURFACE = window->wlSurface() ? window->wlSurface()->resource() : nullptr;
    return window == m_focusedWindow.lock() || (SURFACE && SURFACE == m_keyboardFocus.lock());
}

void CAgentSeat::setSelection(SP<IDataSource> source, bool primary) {
    auto& slot = primary ? m_primarySelection : m_selection;

    if (slot.source == source)
        return;

    slot.destroy.reset();

    if (const auto OLD = slot.source.lock(); OLD)
        OLD->cancelled();

    slot.source = source;

    if (source)
        slot.destroy = source->m_events.destroy.listen([this, primary] { setSelection(nullptr, primary); });

    if (primary)
        PROTO::primarySelection->invalidateOffersFor(m_actor);
    else
        PROTO::data->invalidateOffersFor(m_actor);

    const auto TARGET = primary ? m_pointerFocus.lock() : m_keyboardFocus.lock();
    if (TARGET)
        sendSelection(TARGET->client(), primary);
}

void CAgentSeat::sendSelection(const wl_client* client, bool primary) {
    if (!client)
        return;

    const auto SOURCE = primary ? m_primarySelection.source.lock() : m_selection.source.lock();

    if (primary)
        PROTO::primarySelection->sendSelectionToClient(client, SOURCE);
    else
        PROTO::data->sendSelectionToClient(client, SOURCE);
}

void CAgentSeat::addPopupGrab(SP<CXDGPopupResource> popup) {
    if (!popup || !popup->m_surface)
        return;

    const auto SURFACE = popup->m_surface->m_surface.lock();
    if (!SURFACE || controllerOf(SURFACE) != m_actor) {
        popup->done();
        return;
    }

    popupGrabActive();

    m_grabbedPopups.emplace_back(popup);
    m_popupGrab->add(SURFACE);
    if (popup->m_parent)
        m_popupGrab->add(popup->m_parent->m_surface.lock());

    setKeyboardSurface(SURFACE);
}

void CAgentSeat::endPopupGrab() {
    // Copy first: done() may destroy popups, which prunes the list.
    const auto POPUPS = std::exchange(m_grabbedPopups, {});
    m_popupGrab->clear();

    for (const auto& popup : POPUPS | std::views::reverse) {
        if (const auto PPOPUP = popup.lock(); PPOPUP)
            PPOPUP->done();
    }

    const auto WINDOW = m_focusedWindow.lock();
    if (Desktop::View::validMapped(WINDOW))
        setKeyboardSurface(WINDOW->wlSurface()->resource());
}

void CAgentSeat::releaseClient(const wl_client* client) {
    if (!client)
        return;

    bool popupOfClient = false;
    for (const auto& popup : m_grabbedPopups) {
        const auto PPOPUP = popup.lock();
        if (PPOPUP && PPOPUP->m_surface && PPOPUP->m_surface->m_surface && PPOPUP->m_surface->m_surface->client() == client)
            popupOfClient = true;
    }

    if (popupOfClient)
        endPopupGrab();

    if (const auto WINDOW = m_focusedWindow.lock(); WINDOW && WINDOW->wlSurface()->resource() && WINDOW->wlSurface()->resource()->client() == client)
        focusWindow(nullptr);

    if (const auto FOCUS = m_keyboardFocus.lock(); FOCUS && FOCUS->client() == client)
        setKeyboardSurface(nullptr);

    if (const auto FOCUS = m_pointerFocus.lock(); FOCUS && FOCUS->client() == client) {
        m_pointerWindow.reset();
        setPointerFocus(nullptr, {});
    }
}
