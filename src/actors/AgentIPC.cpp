#include "AgentIPC.hpp"

#include "Actors.hpp"
#include "AgentManager.hpp"
#include "AgentSeat.hpp"

#include "../debug/log/Logger.hpp"
#include "../desktop/state/WindowState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../ipc/s1/S1.hpp"
#include "../ipc/s2/S2.hpp"
#include "../output/Monitor.hpp"
#include "../workspace/HLWorkspace.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

using namespace Actors;
using namespace IPC::Socket1;

static std::string requestPath(const std::string& directory) {
    return directory + "/.socket.sock";
}

static std::string eventPath(const std::string& directory) {
    return directory + "/.socket2.sock";
}

static std::optional<uintptr_t> parseAddress(std::string_view value) {
    if (value.starts_with("address:"))
        value.remove_prefix(8);
    if (value.starts_with("0x"))
        value.remove_prefix(2);

    uintptr_t  address = 0;
    const auto RESULT  = std::from_chars(value.data(), value.data() + value.size(), address, 16);
    if (RESULT.ec != std::errc{} || RESULT.ptr != value.data() + value.size())
        return std::nullopt;

    return address;
}

static std::optional<Vector2D> parsePosition(std::string_view value) {
    const auto SEPARATOR = value.find(' ');
    if (SEPARATOR == std::string_view::npos)
        return std::nullopt;

    const auto X = value.substr(0, SEPARATOR);
    const auto Y = value.substr(SEPARATOR + 1);

    double     x = 0, y = 0;
    const auto RX = std::from_chars(X.data(), X.data() + X.size(), x);
    const auto RY = std::from_chars(Y.data(), Y.data() + Y.size(), y);
    if (RX.ec != std::errc{} || RX.ptr != X.data() + X.size() || RY.ec != std::errc{} || RY.ptr != Y.data() + Y.size())
        return std::nullopt;

    return Vector2D{x, y};
}

static std::vector<PHLWINDOW> controlledWindows(ActorID actor) {
    std::vector<PHLWINDOW> windows;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w->mapped() && controllerOf(w) == actor)
            windows.emplace_back(w);
    }

    return windows;
}

static PHLWINDOW controlledWindow(ActorID actor, std::string_view address) {
    const auto ADDRESS = parseAddress(address);
    if (!ADDRESS)
        return nullptr;

    for (const auto& w : controlledWindows(actor)) {
        if (rc<uintptr_t>(w.get()) == *ADDRESS)
            return w;
    }

    return nullptr;
}

static CAgentSeat* seatOf(ActorID actor) {
    return g_pAgentManager ? g_pAgentManager->seat(actor) : nullptr;
}

// Positions are relative to the agent's monitor, so every agent sees a desktop at 0,0.
static std::string windowData(const PHLWINDOW& w, bool json, bool focused) {
    const auto MONITOR   = w->m_monitor.lock();
    const auto ORIGIN    = MONITOR ? MONITOR->m_position : Vector2D{};
    const auto POSITION  = w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) - ORIGIN;
    const auto SIZE      = w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    const auto WORKSPACE = w->m_workspace ? w->m_workspace->addressableName() : std::string{};

    if (json) {
        return std::format(R"#({{
    "address": "0x{:x}",
    "at": [{}, {}],
    "size": [{}, {}],
    "workspace": "{}",
    "floating": {},
    "class": "{}",
    "title": "{}",
    "initialClass": "{}",
    "initialTitle": "{}",
    "pid": {},
    "focused": {}
}})#",
                           rc<uintptr_t>(w.get()), sc<int>(POSITION.x), sc<int>(POSITION.y), sc<int>(SIZE.x), sc<int>(SIZE.y), escapeJSONStrings(WORKSPACE),
                           w->isFloating() ? "true" : "false", escapeJSONStrings(w->metadata().appID()), escapeJSONStrings(w->metadata().title()),
                           escapeJSONStrings(w->metadata().initialAppID()), escapeJSONStrings(w->metadata().initialTitle()), w->backend().pid(), focused ? "true" : "false");
    }

    return std::format("Window {:x} -> {}:\n\tat: {},{}\n\tsize: {},{}\n\tworkspace: {}\n\tfloating: {}\n\tclass: {}\n\ttitle: {}\n\tinitialClass: {}\n\tinitialTitle: "
                       "{}\n\tpid: {}\n\tfocused: {}\n\n",
                       rc<uintptr_t>(w.get()), w->metadata().title(), sc<int>(POSITION.x), sc<int>(POSITION.y), sc<int>(SIZE.x), sc<int>(SIZE.y), WORKSPACE,
                       sc<int>(w->isFloating()), w->metadata().appID(), w->metadata().title(), w->metadata().initialAppID(), w->metadata().initialTitle(), w->backend().pid(),
                       sc<int>(focused));
}

// The agent's monitor, at 0,0 like every agent monitor its clients see.
static std::string monitorData(const PHLMONITOR& m, bool json) {
    const auto WORKSPACE = m->m_activeWorkspace ? m->m_activeWorkspace->addressableName() : std::string{};

    if (json) {
        return std::format(R"#([{{
    "id": {},
    "name": "{}",
    "width": {},
    "height": {},
    "refreshRate": {:.5f},
    "x": 0,
    "y": 0,
    "scale": {:.2f},
    "transform": {},
    "activeWorkspace": "{}",
    "focused": true
}}])#",
                           m->m_id, escapeJSONStrings(m->m_name), sc<int>(m->m_pixelSize.x), sc<int>(m->m_pixelSize.y), m->m_refreshRate, m->m_scale, sc<int>(m->m_transform),
                           escapeJSONStrings(WORKSPACE));
    }

    return std::format("Monitor {} (ID {}):\n\t{}x{}@{:.5f} at 0x0\n\tactive workspace: {}\n\tscale: {:.2f}\n\ttransform: {}\n\tfocused: yes\n\n", m->m_name, m->m_id,
                       sc<int>(m->m_pixelSize.x), sc<int>(m->m_pixelSize.y), m->m_refreshRate, WORKSPACE, m->m_scale, sc<int>(m->m_transform));
}

std::expected<UP<CAgentIPC>, std::string> CAgentIPC::create(ActorID actor, std::string directory) {
    auto ipc = UP<CAgentIPC>(new CAgentIPC(actor, std::move(directory)));

    if (mkdir(ipc->m_directory.c_str(), S_IRWXU) == 0)
        ipc->m_createdDirectory = true;
    else if (errno != EEXIST)
        return std::unexpected(std::format("cannot create {}: {}", ipc->m_directory, std::strerror(errno)));

    // A crashed instance with the same signature can leave sockets behind, and binding to an
    // existing path fails.
    unlink(requestPath(ipc->m_directory).c_str());
    unlink(eventPath(ipc->m_directory).c_str());

    ipc->m_requests = makeUnique<CSocket1>(ipc->m_directory, false);
    ipc->m_events   = makeUnique<IPC::Socket2::CSocket2>(ipc->m_directory);
    ipc->registerCommands();

    // The socket classes log their own errors. A socket that did not bind has no file.
    for (const auto& path : {requestPath(ipc->m_directory), eventPath(ipc->m_directory)}) {
        if (access(path.c_str(), F_OK) != 0)
            return std::unexpected(std::format("cannot listen on {}", path));
    }

    LOG(Log::DEBUG, "{} IPC listens in {}", nameOf(actor), ipc->m_directory);
    return ipc;
}

CAgentIPC::CAgentIPC(ActorID actor, std::string directory) : m_actor(actor), m_directory(std::move(directory)) {
    ;
}

CAgentIPC::~CAgentIPC() {
    // Socket1 removes its socket file; socket2 leaves its own behind.
    m_requests.reset();
    m_events.reset();
    unlink(eventPath(m_directory).c_str());

    if (m_createdDirectory)
        rmdir(m_directory.c_str());
}

void CAgentIPC::postEvent(const IPC::Socket2::SEvent& event) {
    if (m_events)
        m_events->send(event);
}

void CAgentIPC::registerCommands() {
    const auto ACTOR = m_actor;

    m_requests->registerCommand({
        .name    = "monitors",
        .match   = COMMAND_MATCH_EXACT,
        .handler = [ACTOR](const SRequest& request) -> SResponse {
            const bool JSON    = request.format == FORMAT_JSON;
            const auto MONITOR = monitorOf(ACTOR);
            if (!MONITOR)
                return JSON ? "[]" : "no monitor";

            return monitorData(MONITOR, JSON);
        },
    });

    m_requests->registerCommand({
        .name    = "clients",
        .match   = COMMAND_MATCH_EXACT,
        .handler = [ACTOR](const SRequest& request) -> SResponse {
            const bool  JSON    = request.format == FORMAT_JSON;
            const auto  SEAT    = seatOf(ACTOR);
            const auto  FOCUSED = SEAT ? SEAT->focusedWindow() : nullptr;

            std::string result = JSON ? "[" : "";
            bool        first  = true;
            for (const auto& w : controlledWindows(ACTOR)) {
                if (JSON && !first)
                    result += ",";
                result += windowData(w, JSON, w == FOCUSED);
                first = false;
            }

            if (JSON)
                result += "]";

            return result;
        },
    });

    m_requests->registerCommand({
        .name    = "activewindow",
        .match   = COMMAND_MATCH_EXACT,
        .handler = [ACTOR](const SRequest& request) -> SResponse {
            const bool JSON    = request.format == FORMAT_JSON;
            const auto SEAT    = seatOf(ACTOR);
            const auto FOCUSED = SEAT ? SEAT->focusedWindow() : nullptr;
            if (!FOCUSED || controllerOf(FOCUSED) != ACTOR)
                return JSON ? "{}" : "Invalid";

            return windowData(FOCUSED, JSON, true);
        },
    });

    m_requests->registerCommand({
        .name    = "dispatch ",
        .match   = COMMAND_MATCH_PREFIX,
        .handler = [ACTOR](const SRequest& request) -> SResponse {
            const auto ARGS       = std::string_view{request.command}.substr(9);
            const auto SEPARATOR  = ARGS.find(' ');
            const auto DISPATCHER = ARGS.substr(0, SEPARATOR);
            const auto VALUE      = SEPARATOR == std::string_view::npos ? std::string_view{} : ARGS.substr(SEPARATOR + 1);
            const auto SEAT       = seatOf(ACTOR);
            if (!SEAT)
                return "the agent has no seat";

            // Positions are monitor-local, the same coordinates "clients" reports.
            if (DISPATCHER == "movecursor") {
                const auto POSITION = parsePosition(VALUE);
                if (!POSITION)
                    return "movecursor takes X Y";

                SEAT->warpPointer(*POSITION);
                return "ok";
            }

            if (DISPATCHER != "focuswindow" && DISPATCHER != "closewindow")
                return std::format("unknown dispatcher {}", DISPATCHER);

            const auto WINDOW = controlledWindow(ACTOR, VALUE);
            if (!WINDOW)
                return "no such window";

            if (DISPATCHER == "focuswindow")
                SEAT->focusWindow(WINDOW);
            else
                WINDOW->sendClose();

            return "ok";
        },
    });
}
