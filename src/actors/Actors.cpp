#include "Actors.hpp"

#include "ActorRegistry.hpp"
#include "AgentManager.hpp"

#include "../debug/log/Logger.hpp"
#include "../desktop/view/LayerSurface.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../output/Monitor.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../state/MonitorState.hpp"
#include "../workspace/HLWorkspace.hpp"

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>

#include <cstdlib>
#include <format>
#include <type_traits>
#include <variant>

#include <wayland-server-core.h>

/*
    Lifecycle, identity and desktops. The rules live in ActorPolicy.cpp.
*/

void Actors::init(wl_display* display) {
    g_pActorRegistry = makeUnique<CActorRegistry>(display);
}

std::expected<void, std::string> Actors::createAgentSockets(const std::string& humanSocket) {
    if (!g_pActorRegistry)
        return std::unexpected("actors are not initialized");

    g_pActorRegistry->setHumanSocket(humanSocket);

    const auto COUNT_ENV = getenv("HYPRLAND_AGENT_COUNT");
    if (!COUNT_ENV)
        return {};

    const auto COUNT = parseAgentCount(COUNT_ENV);
    if (!COUNT)
        return std::unexpected(std::format("HYPRLAND_AGENT_COUNT is invalid: {}", COUNT.error()));

    const auto RUNTIME_DIR = getenv("XDG_RUNTIME_DIR");
    return g_pActorRegistry->createAgentSockets(*COUNT, RUNTIME_DIR ? RUNTIME_DIR : "");
}

void Actors::start() {
    if (!g_pActorRegistry || g_pAgentManager)
        return;

    g_pAgentManager = makeUnique<CAgentManager>();
    g_pAgentManager->start();
}

void Actors::destroy() {
    g_pAgentManager.reset();
    g_pActorRegistry.reset();
}

wl_client* Actors::createClient(wl_display* display, int fd, ActorID owner) {
    if (!g_pActorRegistry)
        return wl_client_create(display, fd);

    return g_pActorRegistry->createClient(fd, owner);
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

// Only a headless output with a reserved agent name becomes an agent monitor. A real monitor
// that happens to use the same name stays human.
Actors::ActorID Actors::actorForOutput(const SP<Aquamarine::IOutput>& output) {
    if (!g_pActorRegistry || !output)
        return HUMAN_ACTOR_ID;

    const auto BACKEND = output->getBackend();
    if (!BACKEND || BACKEND->type() != Aquamarine::AQ_BACKEND_HEADLESS)
        return HUMAN_ACTOR_ID;

    return g_pActorRegistry->actorForOutputName(output->name).value_or(HUMAN_ACTOR_ID);
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

std::string Actors::nameOf(ActorID id) {
    if (!g_pActorRegistry)
        return id == HUMAN_ACTOR_ID ? "human" : "unknown";

    const auto ACTOR = g_pActorRegistry->actor(id);
    return ACTOR ? ACTOR->name : "unknown";
}

bool Actors::isReservedOutputName(std::string_view name) {
    return g_pActorRegistry && g_pActorRegistry->isReservedOutputName(name);
}

bool Actors::isReservedWorkspaceName(std::string_view name, ActorID actor) {
    if (!g_pActorRegistry)
        return false;

    const auto OWNER = g_pActorRegistry->actorByName(name);
    return OWNER && OWNER->kind == eActorKind::AGENT && OWNER->id != actor;
}

Actors::ActorID Actors::desktopOf(const PHLWINDOW& window) {
    if (!window)
        return HUMAN_ACTOR_ID;

    // The surface is gone, as on destroy: the window is where its workspace is.
    if ((!window->wlSurface() || !window->wlSurface()->resource()) && window->m_workspace)
        return actorOf(window->m_workspace);

    return controllerOf(window);
}

Actors::ActorID Actors::desktopOf(const PHLWORKSPACE& workspace) {
    return actorOf(workspace);
}

Actors::ActorID Actors::desktopOf(const PHLMONITOR& monitor) {
    return actorOf(monitor);
}

Actors::ActorID Actors::desktopOf(const PHLLS& layer) {
    return layer ? actorOf(layer->m_monitor.lock()) : HUMAN_ACTOR_ID;
}

bool Actors::humanSees(const PHLMONITOR& monitor) {
    return desktopOf(monitor) == HUMAN_ACTOR_ID;
}

bool Actors::humanSees(const PHLWORKSPACE& workspace) {
    return desktopOf(workspace) == HUMAN_ACTOR_ID;
}

bool Actors::humanSees(const PHLWINDOW& window) {
    return window && desktopOf(window) == HUMAN_ACTOR_ID;
}

bool Actors::humanSees(const PHLLS& layer) {
    return layer && desktopOf(layer) == HUMAN_ACTOR_ID;
}

PHLMONITOR Actors::monitorOf(ActorID agent) {
    return g_pAgentManager ? g_pAgentManager->monitorFor(agent) : nullptr;
}

void Actors::placeAgentMonitors() {
    for (const auto& m : State::monitorState()->monitors()) {
        if (!isAgentMonitor(m))
            continue;

        const Vector2D POS = {AGENT_LAYOUT_ORIGIN_X + (m->m_actor - 1) * AGENT_LAYOUT_STRIDE, AGENT_LAYOUT_ORIGIN_Y};
        m->moveTo(POS);
        m->setXWaylandPosition(POS);
    }
}

Vector2D Actors::outputPosition(const PHLMONITOR& monitor, const Vector2D& position) {
    return isAgentMonitor(monitor) ? Vector2D{} : position;
}

Actors::ActorID Actors::eventAudience(const SEventSubject& subject) {
    return std::visit(
        [](const auto& x) -> ActorID {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return HUMAN_ACTOR_ID;
            else if constexpr (std::is_same_v<T, ActorID>)
                return x;
            else
                return desktopOf(x);
        },
        subject);
}

void Actors::postAgentEvent(ActorID agent, const IPC::Socket2::SEvent& event) {
    if (g_pAgentManager)
        g_pAgentManager->postEvent(agent, event);
}
