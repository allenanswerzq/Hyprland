#include "AgentManager.hpp"

#include "ActorRegistry.hpp"
#include "Actors.hpp"
#include "AgentIPC.hpp"
#include "AgentSeat.hpp"

#include "../Compositor.hpp"
#include "../config/shared/actions/ConfigActions.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/history/WorkspaceHistoryTracker.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/state/GlobalWindowController.hpp"
#include "../desktop/state/WindowState.hpp"
#include "../desktop/view/Group.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../desktop/view/window/WindowGroupMembership.hpp"
#include "../desktop/view/window/WindowPresentation.hpp"
#include "../devices/IKeyboard.hpp"
#include "../event/EventBus.hpp"
#include "../ipc/s2/S2.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../output/Monitor.hpp"
#include "../protocols/PrimarySelection.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/core/Output.hpp"
#include "../protocols/core/Seat.hpp"
#include "../protocols/types/DataDevice.hpp"
#include "../state/MonitorState.hpp"
#include "../state/WorkspaceState.hpp"
#include "../workspace/HLWorkspace.hpp"

#include <aquamarine/backend/Backend.hpp>
#include <wayland-server-protocol.h>

#include <algorithm>
#include <format>

using namespace Actors;

// Offers the client already got belong to the clipboard of its old controller.
static void invalidateClipboardOffers(const wl_client* client) {
    if (PROTO::data)
        PROTO::data->invalidateOffersForClient(client);
    if (PROTO::primarySelection)
        PROTO::primarySelection->invalidateOffersForClient(client);
}

static PHLMONITOR humanMonitor() {
    if (const auto FOCUSED = Desktop::focusState()->monitor())
        return FOCUSED;

    const auto& MONITORS = State::monitorState()->monitors();
    return MONITORS.empty() ? nullptr : MONITORS.front();
}

static wl_client* clientOf(const PHLWINDOW& window) {
    if (!window || window->backend().isX11() || !window->wlSurface())
        return nullptr;

    const auto SURFACE = window->wlSurface()->resource();
    return SURFACE ? SURFACE->client() : nullptr;
}

// After a client changed controller, its seat follows the other actor's devices: resend the
// capabilities, keymap and repeat info.
static void refreshSeat(const wl_client* client) {
    if (!PROTO::seat)
        return;

    const auto CAPS     = PROTO::seat->capsForClient(client);
    const auto KEYBOARD = keyboardFor(client);

    for (const auto& seat : PROTO::seat->seatResourcesForClient(client)) {
        seat->sendCapabilities(CAPS);

        if (!KEYBOARD)
            continue;

        for (const auto& keyboard : seat->m_keyboards) {
            if (!keyboard)
                continue;

            keyboard->sendKeymap(KEYBOARD);
            keyboard->repeatInfo(KEYBOARD->m_repeatRate, KEYBOARD->m_repeatDelay);
        }
    }
}

// Window rules can match a borrowed window by tag:borrowed or by its agent's name.
static void setBorrowTags(const PHLWINDOW& window, ActorID owner, bool borrowed) {
    bool changed = false;
    for (const auto& tag : borrowTags(owner)) {
        changed |= window->m_ruleApplicator->m_tagKeeper.applyTag((borrowed ? "+" : "-") + tag);
    }

    if (changed && window->mapped()) {
        window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        window->presentation().refreshValues();
    }
}

// An empty borrow workspace on screen gives way to the workspace its monitor showed before,
// as "workspace previous" does. Hyprland removes the empty workspace once it is off screen.
static void leaveEmptyWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || workspace->getWindowCount() > 0)
        return;

    const auto MONITOR = workspace->m_monitor.lock();
    if (!MONITOR || MONITOR->m_activeWorkspace != workspace)
        return;

    const auto PREVIOUS = Desktop::History::workspaceTracker()->previousWorkspace(workspace, MONITOR);
    auto       next     = PREVIOUS.workspace.lock();
    if (!next && PREVIOUS.target.valid()) {
        next = State::workspaceState()->find(PREVIOUS.target);
        if (!next)
            next = State::workspaceState()->create(PREVIOUS.target, MONITOR);
    }

    if (!next || next == workspace)
        return;

    Config::Actions::changeWorkspace(next, MONITOR != Desktop::focusState()->monitor());
    next->ready();
}

CAgentManager::CAgentManager() {
    if (g_pActorRegistry) {
        for (const auto& actor : g_pActorRegistry->actors()) {
            if (actor.kind == eActorKind::AGENT)
                m_seats[actor.id] = makeUnique<CAgentSeat>(actor.id);
        }

        m_listeners.clientDestroyed = g_pActorRegistry->m_events.clientDestroyed.listen([this](wl_client* client) { onClientDestroyed(client); });
    }

    m_listeners.monitorAdded   = Event::bus()->m_events.monitor.added.listen([this](const PHLMONITOR& monitor) { onMonitorAdded(monitor); });
    m_listeners.monitorRemoved = Event::bus()->m_events.monitor.removed.listen([this](const PHLMONITOR& monitor) { onMonitorRemoved(monitor); });

    // A new window of a borrowed client carries the borrow tags before its rules are read
    // at map, so rules can match tag:borrowed.
    m_listeners.windowCreated = Event::bus()->m_events.window.create.listen([](const PHLWINDOW& window) {
        if (Actors::isBorrowed(window))
            setBorrowTags(window, window->owner(), true);
    });
}

CAgentManager::~CAgentManager() {
    for (auto& borrow : m_borrows) {
        removeProxyOutputs(borrow);
    }

    m_borrows.clear();
    m_ipc.clear();
    m_seats.clear();
}

void CAgentManager::start() {
    if (m_started || !g_pActorRegistry)
        return;

    m_started = true;

    SP<Aquamarine::IBackendImplementation> headless;
    for (const auto& impl : g_pCompositor->m_aqBackend->getImplementations()) {
        if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS) {
            headless = impl;
            break;
        }
    }

    for (const auto& actor : g_pActorRegistry->actors()) {
        if (actor.kind != eActorKind::AGENT)
            continue;

        if (!headless)
            LOG(Log::ERR, "Agent {} has no desktop: there is no headless backend", actor.name);
        else if (!monitorFor(actor.id) && !headless->createOutput(actor.outputName))
            LOG(Log::ERR, "Agent {} has no desktop: cannot create output {}", actor.name, actor.outputName);

        auto ipc = CAgentIPC::create(actor.id, std::format("{}-{}", g_pCompositor->m_instancePath, actor.name));
        if (!ipc) {
            LOG(Log::ERR, "Agent {} has no IPC: {}", actor.name, ipc.error());
            continue;
        }

        m_ipc[actor.id] = std::move(*ipc);
    }
}

CAgentSeat* CAgentManager::seat(ActorID actor) const {
    const auto IT = m_seats.find(actor);
    return IT == m_seats.end() ? nullptr : IT->second.get();
}

PHLMONITOR CAgentManager::monitorFor(ActorID actor) const {
    if (!isAgent(actor))
        return nullptr;

    for (const auto& m : State::monitorState()->agentMonitors()) {
        if (m->m_actor == actor && m->m_enabled)
            return m;
    }

    return nullptr;
}

PHLWORKSPACE CAgentManager::workspaceFor(ActorID actor) const {
    const auto MONITOR = monitorFor(actor);
    return MONITOR ? MONITOR->m_activeWorkspace : nullptr;
}

CAgentManager::SBorrow* CAgentManager::borrowOf(const wl_client* client) {
    const auto IT = std::ranges::find_if(m_borrows, [client](const auto& borrow) { return borrow.client == client; });
    return IT == m_borrows.end() ? nullptr : &*IT;
}

bool CAgentManager::isBorrowed(const wl_client* client) const {
    return std::ranges::any_of(m_borrows, [client](const auto& borrow) { return borrow.client == client; });
}

std::vector<PHLWINDOW> CAgentManager::windowsOf(const wl_client* client) const {
    std::vector<PHLWINDOW> windows;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (clientOf(w) == client)
            windows.emplace_back(w);
    }

    return windows;
}

void CAgentManager::moveClientWindows(const wl_client* client, const PHLWORKSPACE& workspace) {
    for (const auto& w : windowsOf(client)) {
        if (!w->mapped())
            continue;

        // A group belongs to one workspace, and the rest of the group stays behind.
        if (const auto GROUP = w->grouping().group(); GROUP)
            GROUP->remove(w);

        Desktop::globalWindowController()->moveWindowToWorkspace(w, workspace);
    }
}

PHLWORKSPACE CAgentManager::borrowWorkspace(ActorID agent, bool create) {
    if (!isAgent(agent))
        return nullptr;

    const auto NAME = borrowWorkspaceName(agent);
    for (const auto& ref : State::workspaceState()->workspaces()) {
        const auto WS = ref.lock();
        if (WS && WS->type() == Workspace::eWorkspaceType::NORMAL && WS->addressableName() == NAME && !isAgent(actorOf(WS)))
            return WS;
    }

    const auto MONITOR = create ? humanMonitor() : nullptr;
    if (!MONITOR)
        return nullptr;

    return State::workspaceState()->createNamed(NAME, MONITOR, NAME, false);
}

std::expected<void, std::string> CAgentManager::borrow(PHLWINDOW window) {
    if (!window || !window->mapped())
        return std::unexpected("no such window");

    if (window->backend().isX11())
        return std::unexpected("X11 windows cannot be borrowed");

    const auto CLIENT = clientOf(window);
    if (!CLIENT)
        return std::unexpected("the window has no Wayland client");

    const auto OWNER = ownerOf(CLIENT);
    if (!OWNER || !isAgent(*OWNER))
        return std::unexpected("the window does not belong to an agent");

    if (isBorrowed(CLIENT))
        return std::unexpected("the window is already borrowed");

    const auto WORKSPACE = borrowWorkspace(*OWNER, true);
    if (!WORKSPACE)
        return std::unexpected("there is no human workspace to show the window on");

    // The agent seat lets go first, so no agent focus or grab points at a human client.
    if (const auto SEAT = seat(*OWNER); SEAT)
        SEAT->releaseClient(CLIENT);

    if (!g_pActorRegistry->setController(CLIENT, HUMAN_ACTOR_ID))
        return std::unexpected("cannot change the controller of the window's client");

    invalidateClipboardOffers(CLIENT);
    refreshSeat(CLIENT);

    m_borrows.emplace_back(SBorrow{.client = CLIENT, .owner = *OWNER});
    for (const auto& m : State::monitorState()->monitors()) {
        addProxyOutput(m_borrows.back(), m);
    }

    const auto WINDOWS = windowsOf(CLIENT);
    moveClientWindows(CLIENT, WORKSPACE);

    for (const auto& w : WINDOWS) {
        setBorrowTags(w, *OWNER, true);
        IPC::Socket2::sock()->postEvent({.event = "borrowwindow", .data = std::format("{:x}", rc<uintptr_t>(w.get())), .subject = *OWNER});
    }

    // The human goes to the borrow workspace, on whichever monitor it is.
    WORKSPACE->ready();
    Config::Actions::changeWorkspace(WORKSPACE);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);

    LOG(Log::DEBUG, "The human borrowed client {:x} of agent {}", rc<uintptr_t>(CLIENT), *OWNER);
    return {};
}

std::expected<void, std::string> CAgentManager::returnWindow(PHLWINDOW window) {
    const auto CLIENT = clientOf(window);
    const auto BORROW = CLIENT ? borrowOf(CLIENT) : nullptr;
    if (!BORROW)
        return std::unexpected("the window is not borrowed");

    const auto OWNER            = BORROW->owner;
    const auto WORKSPACE        = workspaceFor(OWNER);
    const auto BORROW_WORKSPACE = borrowWorkspace(OWNER, false);
    if (!WORKSPACE)
        return std::unexpected("the agent's desktop is not available");

    // The human seat lets go first, so no human focus or grab points at an agent client. The
    // grab ends first, because ending a popup grab can focus the popup's parent again.
    PROTO::xdgShell->dismissGrabsFor(CLIENT);

    if (const auto FOCUSED = Desktop::focusState()->window(); FOCUSED && clientOf(FOCUSED) == CLIENT)
        Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);

    if (const auto FOCUS = g_pSeatManager->m_state.keyboardFocus.lock(); FOCUS && FOCUS->client() == CLIENT)
        g_pSeatManager->setKeyboardFocus(nullptr);

    if (const auto FOCUS = g_pSeatManager->m_state.pointerFocus.lock(); FOCUS && FOCUS->client() == CLIENT)
        g_pSeatManager->setPointerFocus(nullptr, {});

    if (!g_pActorRegistry->setController(CLIENT, OWNER))
        return std::unexpected("cannot change the controller of the window's client");

    invalidateClipboardOffers(CLIENT);
    refreshSeat(CLIENT);

    const auto WINDOWS = windowsOf(CLIENT);
    moveClientWindows(CLIENT, WORKSPACE);

    // The proxies go last: the windows left the human monitors through them.
    removeProxyOutputs(*BORROW);
    std::erase_if(m_borrows, [CLIENT](const auto& other) { return other.client == CLIENT; });

    for (const auto& w : WINDOWS) {
        setBorrowTags(w, OWNER, false);
        IPC::Socket2::sock()->postEvent({.event = "returnwindow", .data = std::format("{:x}", rc<uintptr_t>(w.get())), .subject = OWNER});
    }

    leaveEmptyWorkspace(BORROW_WORKSPACE);
    g_pInputManager->refocus();

    LOG(Log::DEBUG, "The human returned client {:x} to agent {}", rc<uintptr_t>(CLIENT), OWNER);
    return {};
}

void CAgentManager::addProxyOutput(SBorrow& borrow, const PHLMONITOR& monitor) {
    if (!monitor || isAgentMonitor(monitor) || monitor->isMirror())
        return;

    if (std::ranges::any_of(borrow.outputs, [&monitor](const auto& proxy) { return proxy.monitor == monitor; }))
        return;

    // Only the borrowed client sees the proxy.
    auto output    = makeShared<CWLOutputProtocol>(&wl_output_interface, 4, std::format("WLOutput proxy ({})", monitor->m_name), monitor,
                                                   [client = borrow.client](const wl_client* other) { return other == client; });
    output->m_self = output;

    borrow.outputs.emplace_back(SProxyOutput{.output = output, .monitor = monitor});
}

void CAgentManager::removeProxyOutputs(SBorrow& borrow, const PHLMONITOR& onlyMonitor) {
    for (auto& proxy : borrow.outputs) {
        const auto MONITOR = proxy.monitor.lock();
        if (onlyMonitor && MONITOR != onlyMonitor)
            continue;

        // Leave through the proxy while the client can still resolve it.
        if (MONITOR && PROTO::compositor) {
            PROTO::compositor->forEachSurface([&borrow, &MONITOR](SP<CWLSurfaceResource> surface) {
                if (surface->client() == borrow.client)
                    surface->leave(MONITOR);
            });
        }

        // The global keeps its audience, so global_remove reaches only the borrowed client. A
        // retired output lives on until the client released it.
        proxy.output->remove();

        if (proxy.output->hasBoundResources())
            PROTO::defunctOutputs.emplace_back(proxy.output);

        proxy.output.reset();
    }

    std::erase_if(borrow.outputs, [](const auto& proxy) { return !proxy.output; });
}

std::vector<SP<CWLOutputResource>> CAgentManager::proxyOutputResources(const PHLMONITOR& monitor, wl_client* client) const {
    std::vector<SP<CWLOutputResource>> resources;

    for (const auto& borrow : m_borrows) {
        if (borrow.client != client)
            continue;

        for (const auto& proxy : borrow.outputs) {
            if (proxy.output && proxy.monitor == monitor) {
                for (auto& resource : proxy.output->outputResourcesFrom(client))
                    resources.emplace_back(std::move(resource));
            }
        }
    }

    return resources;
}

void CAgentManager::onMonitorAdded(const PHLMONITOR& monitor) {
    if (isAgentMonitor(monitor)) {
        if (const auto SEAT = seat(monitor->m_actor); SEAT)
            SEAT->onMonitorChanged();
        return;
    }

    for (auto& borrow : m_borrows) {
        addProxyOutput(borrow, monitor);
    }
}

void CAgentManager::onMonitorRemoved(const PHLMONITOR& monitor) {
    if (isAgentMonitor(monitor)) {
        LOG(Log::ERR, "Agent monitor {} was removed. The agent's windows have no desktop until it comes back.", monitor->m_name);
        return;
    }

    for (auto& borrow : m_borrows) {
        removeProxyOutputs(borrow, monitor);
    }
}

void CAgentManager::onClientDestroyed(wl_client* client) {
    // All the client's resources are gone already, so the proxies have nothing bound.
    if (const auto BORROW = borrowOf(client); BORROW)
        removeProxyOutputs(*BORROW);

    std::erase_if(m_borrows, [client](const auto& borrow) { return borrow.client == client; });
}

void CAgentManager::postEvent(ActorID actor, const IPC::Socket2::SEvent& event) {
    if (const auto IT = m_ipc.find(actor); IT != m_ipc.end())
        IT->second->postEvent(event);
}
