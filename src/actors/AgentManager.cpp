#include "AgentManager.hpp"

#include "ActorRegistry.hpp"
#include "Actors.hpp"
#include "AgentIPC.hpp"
#include "AgentSeat.hpp"

#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/state/GlobalWindowController.hpp"
#include "../desktop/state/WindowState.hpp"
#include "../desktop/view/Group.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../desktop/view/window/WindowGroupMembership.hpp"
#include "../event/EventBus.hpp"
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
    if (const auto FOCUSED = Desktop::focusState()->monitor(); FOCUSED && !isAgentMonitor(FOCUSED))
        return FOCUSED;

    for (const auto& m : State::monitorState()->monitors()) {
        if (!isAgentMonitor(m))
            return m;
    }

    return nullptr;
}

static wl_client* clientOf(const PHLWINDOW& window) {
    if (!window || window->backend().isX11() || !window->wlSurface())
        return nullptr;

    const auto SURFACE = window->wlSurface()->resource();
    return SURFACE ? SURFACE->client() : nullptr;
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

    m_listeners.windowOpen = Event::bus()->m_events.window.open.listen([this](const PHLWINDOW& window) {
        const auto ACTOR = controllerOf(window);
        if (!isAgent(ACTOR))
            return;

        postEvent(ACTOR, "openwindow",
                  std::format("{:x},{},{},{}", rc<uintptr_t>(window.get()), window->m_workspace ? window->m_workspace->addressableName() : std::string{},
                              window->metadata().appID(), window->metadata().title()));
    });

    m_listeners.windowClose = Event::bus()->m_events.window.close.listen([this](const PHLWINDOW& window) {
        const auto ACTOR = controllerOf(window);
        if (isAgent(ACTOR))
            postEvent(ACTOR, "closewindow", std::format("{:x}", rc<uintptr_t>(window.get())));
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

    for (const auto& m : State::monitorState()->monitors()) {
        if (m->m_actor == actor)
            return m;
    }

    return nullptr;
}

PHLWORKSPACE CAgentManager::workspaceFor(ActorID actor) const {
    const auto MONITOR = monitorFor(actor);
    return MONITOR ? MONITOR->m_activeWorkspace : nullptr;
}

void CAgentManager::addVirtualKeyboard(ActorID actor, SP<IKeyboard> keyboard) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->addKeyboard(keyboard);
    else
        LOG(Log::ERR, "Virtual keyboard for actor {} without an agent seat", actor);
}

void CAgentManager::addVirtualPointer(ActorID actor, SP<IPointer> pointer) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->addPointer(pointer);
    else
        LOG(Log::ERR, "Virtual pointer for actor {} without an agent seat", actor);
}

SP<IKeyboard> CAgentManager::keyboardFor(ActorID actor) const {
    const auto SEAT = seat(actor);
    return SEAT ? SEAT->keyboard() : nullptr;
}

SP<CWLSurfaceResource> CAgentManager::keyboardFocus(ActorID actor) const {
    const auto SEAT = seat(actor);
    return SEAT ? SEAT->keyboardFocus() : nullptr;
}

SP<CWLSurfaceResource> CAgentManager::pointerFocus(ActorID actor) const {
    const auto SEAT = seat(actor);
    return SEAT ? SEAT->pointerFocus() : nullptr;
}

void CAgentManager::focusWindow(ActorID actor, PHLWINDOW window, SP<CWLSurfaceResource> surface) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->focusWindow(window, surface);
}

void CAgentManager::focusSurface(ActorID actor, SP<CWLSurfaceResource> surface) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->focusSurface(surface);
}

void CAgentManager::warpPointer(ActorID actor, const Vector2D& local) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->warpPointer(local);
}

PHLWINDOW CAgentManager::focusedWindow(ActorID actor) const {
    const auto SEAT = seat(actor);
    return SEAT ? SEAT->focusedWindow() : nullptr;
}

bool CAgentManager::isWindowActive(const PHLWINDOW& window) const {
    const auto SEAT = seat(controllerOf(window));
    return SEAT && SEAT->isWindowActive(window);
}

void CAgentManager::setSelection(ActorID actor, SP<IDataSource> source, bool primary) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->setSelection(source, primary);
    else if (source)
        source->cancelled();
}

void CAgentManager::addPopupGrab(ActorID actor, SP<CXDGPopupResource> popup) {
    if (const auto SEAT = seat(actor); SEAT)
        SEAT->addPopupGrab(popup);
    else if (popup)
        popup->done();
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

    const auto MONITOR   = humanMonitor();
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : nullptr;
    if (!WORKSPACE)
        return std::unexpected("there is no human workspace to show the window on");

    // The agent seat lets go first, so no agent focus or grab points at a human client.
    if (const auto SEAT = seat(*OWNER); SEAT)
        SEAT->releaseClient(CLIENT);

    if (!g_pActorRegistry->setController(CLIENT, HUMAN_ACTOR_ID))
        return std::unexpected("cannot change the controller of the window's client");

    invalidateClipboardOffers(CLIENT);
    PROTO::seat->refreshClient(CLIENT);

    m_borrows.emplace_back(SBorrow{.client = CLIENT, .owner = *OWNER});
    for (const auto& m : State::monitorState()->monitors()) {
        addProxyOutput(m_borrows.back(), m);
    }

    const auto WINDOWS = windowsOf(CLIENT);
    moveClientWindows(CLIENT, WORKSPACE);

    for (const auto& w : WINDOWS) {
        postEvent(*OWNER, "borrowwindow", std::format("{:x}", rc<uintptr_t>(w.get())));
    }

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);

    LOG(Log::DEBUG, "The human borrowed client {:x} of agent {}", rc<uintptr_t>(CLIENT), *OWNER);
    return {};
}

std::expected<void, std::string> CAgentManager::returnWindow(PHLWINDOW window) {
    const auto CLIENT = clientOf(window);
    const auto BORROW = CLIENT ? borrowOf(CLIENT) : nullptr;
    if (!BORROW)
        return std::unexpected("the window is not borrowed");

    const auto OWNER     = BORROW->owner;
    const auto WORKSPACE = workspaceFor(OWNER);
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
    PROTO::seat->refreshClient(CLIENT);

    const auto WINDOWS = windowsOf(CLIENT);
    moveClientWindows(CLIENT, WORKSPACE);

    // The proxies go last: the windows left the human monitors through them.
    removeProxyOutputs(*BORROW);
    std::erase_if(m_borrows, [CLIENT](const auto& other) { return other.client == CLIENT; });

    for (const auto& w : WINDOWS) {
        postEvent(OWNER, "returnwindow", std::format("{:x}", rc<uintptr_t>(w.get())));
    }

    g_pInputManager->refocus();

    LOG(Log::DEBUG, "The human returned client {:x} to agent {}", rc<uintptr_t>(CLIENT), OWNER);
    return {};
}

void CAgentManager::addProxyOutput(SBorrow& borrow, const PHLMONITOR& monitor) {
    if (!monitor || isAgentMonitor(monitor) || monitor->isMirror())
        return;

    if (std::ranges::any_of(borrow.outputs, [&monitor](const auto& proxy) { return proxy.monitor == monitor; }))
        return;

    CPendingOutputGlobal scope(borrow.client);

    auto                 output = makeShared<CWLOutputProtocol>(&wl_output_interface, 4, std::format("WLOutput proxy ({})", monitor->m_name), monitor);
    output->m_self              = output;

    borrow.outputs.emplace_back(SProxyOutput{.output = output, .monitor = monitor});
}

void CAgentManager::removeProxyOutputs(SBorrow& borrow, const PHLMONITOR& onlyMonitor) {
    std::erase_if(m_retiredProxies, [](const auto& retired) { return !retired.output; });

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

        // Removing the global sends global_remove, which asks the filter, which still finds
        // the proxy in this borrow.
        proxy.output->remove();

        if (proxy.output->hasBoundResources()) {
            PROTO::defunctOutputs.emplace_back(proxy.output);
            m_retiredProxies.emplace_back(SRetiredProxy{.output = proxy.output, .client = borrow.client});
        }

        proxy.output.reset();
    }

    std::erase_if(borrow.outputs, [](const auto& proxy) { return !proxy.output; });
}

std::optional<const wl_client*> CAgentManager::proxyOutputClient(const wl_global* global) const {
    for (const auto& borrow : m_borrows) {
        for (const auto& proxy : borrow.outputs) {
            if (proxy.output && proxy.output->getGlobal() == global)
                return borrow.client;
        }
    }

    for (const auto& retired : m_retiredProxies) {
        if (const auto OUTPUT = retired.output.lock(); OUTPUT && OUTPUT->getGlobal() == global)
            return retired.client;
    }

    return std::nullopt;
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
    std::erase_if(m_retiredProxies, [client](const auto& retired) { return retired.client == client || !retired.output; });
}

void CAgentManager::postEvent(ActorID actor, const std::string& event, const std::string& data) {
    if (const auto IT = m_ipc.find(actor); IT != m_ipc.end())
        IT->second->postEvent(event, data);
}
