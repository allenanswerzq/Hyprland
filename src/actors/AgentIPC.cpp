#include "AgentIPC.hpp"

#include "Actors.hpp"
#include "AgentManager.hpp"

#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/state/WindowState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../helpers/time/Time.hpp"
#include "../output/Monitor.hpp"
#include "../workspace/HLWorkspace.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>

using namespace Actors;
using namespace Hyprutils::OS;

// A socket2 listener that stops reading is dropped once this much is queued for it.
constexpr size_t   MAX_EVENT_QUEUE = 1024 * 1024;
constexpr size_t   MAX_REQUEST     = 8192;

static std::string errnoString() {
    return std::strerror(errno);
}

static std::expected<CFileDescriptor, std::string> listenOn(const std::string& path) {
    sockaddr_un address = {};
    address.sun_family  = AF_UNIX;

    if (path.size() >= sizeof(address.sun_path))
        return std::unexpected(std::format("socket path {} is too long", path));

    CFileDescriptor fd{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd.isValid())
        return std::unexpected(std::format("cannot create a socket: {}", errnoString()));

    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str());

    if (bind(fd.get(), rc<sockaddr*>(&address), SUN_LEN(&address)) < 0)
        return std::unexpected(std::format("cannot bind {}: {}", path, errnoString()));

    if (::listen(fd.get(), 16) < 0)
        return std::unexpected(std::format("cannot listen on {}: {}", path, errnoString()));

    return fd;
}

static bool sendAll(int fd, std::string_view data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const auto WRITTEN = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (WRITTEN > 0) {
            offset += sc<size_t>(WRITTEN);
            continue;
        }

        if (WRITTEN < 0 && errno == EINTR)
            continue;

        return false;
    }

    return true;
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

std::expected<UP<CAgentIPC>, std::string> CAgentIPC::create(ActorID actor, std::string directory) {
    auto ipc = UP<CAgentIPC>(new CAgentIPC(actor, std::move(directory)));

    if (auto result = ipc->listen(); !result)
        return std::unexpected(result.error());

    return ipc;
}

CAgentIPC::CAgentIPC(ActorID actor, std::string directory) :
    m_actor(actor), m_directory(std::move(directory)), m_requestPath(m_directory + "/.socket.sock"), m_eventPath(m_directory + "/.socket2.sock") {
    ;
}

CAgentIPC::~CAgentIPC() {
    for (const auto& peer : m_requestPeers) {
        if (peer->source)
            wl_event_source_remove(peer->source);
    }

    for (const auto& peer : m_eventPeers) {
        if (peer->source)
            wl_event_source_remove(peer->source);
    }

    m_requestPeers.clear();
    m_eventPeers.clear();

    if (m_requestSource)
        wl_event_source_remove(m_requestSource);
    if (m_eventSource)
        wl_event_source_remove(m_eventSource);

    if (m_requestSocket.isValid())
        unlink(m_requestPath.c_str());
    if (m_eventSocket.isValid())
        unlink(m_eventPath.c_str());

    m_requestSocket.reset();
    m_eventSocket.reset();

    if (m_createdDirectory)
        rmdir(m_directory.c_str());
}

std::expected<void, std::string> CAgentIPC::listen() {
    if (mkdir(m_directory.c_str(), S_IRWXU) == 0)
        m_createdDirectory = true;
    else if (errno != EEXIST)
        return std::unexpected(std::format("cannot create {}: {}", m_directory, errnoString()));

    auto requestSocket = listenOn(m_requestPath);
    if (!requestSocket)
        return std::unexpected(requestSocket.error());
    m_requestSocket = std::move(*requestSocket);

    auto eventSocket = listenOn(m_eventPath);
    if (!eventSocket)
        return std::unexpected(eventSocket.error());
    m_eventSocket = std::move(*eventSocket);

    m_requestSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_requestSocket.get(), WL_EVENT_READABLE, onRequestSocket, this);
    m_eventSource   = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_eventSocket.get(), WL_EVENT_READABLE, onEventSocket, this);

    if (!m_requestSource || !m_eventSource)
        return std::unexpected("cannot add the agent IPC sockets to the event loop");

    LOG(Log::DEBUG, "Agent {} IPC listens in {}", m_actor, m_directory);
    return {};
}

int CAgentIPC::onRequestSocket(int fd, uint32_t mask, void* data) {
    const auto IPC = sc<CAgentIPC*>(data);

    while (true) {
        CFileDescriptor client{accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (!client.isValid())
            break;

        auto peer    = makeUnique<SRequestPeer>();
        peer->ipc    = IPC;
        peer->fd     = std::move(client);
        peer->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, peer->fd.get(), WL_EVENT_READABLE, onRequestPeer, peer.get());

        if (peer->source)
            IPC->m_requestPeers.emplace_back(std::move(peer));
    }

    return 0;
}

int CAgentIPC::onRequestPeer(int fd, uint32_t mask, void* data) {
    const auto PEER = sc<SRequestPeer*>(data);

    if (mask & WL_EVENT_READABLE)
        PEER->ipc->handleRequest(*PEER);

    PEER->ipc->removeRequestPeer(PEER);
    return 0;
}

void CAgentIPC::handleRequest(SRequestPeer& peer) {
    // hyprctl writes the whole request at once and then waits for the reply.
    std::string request(MAX_REQUEST, '\0');

    ssize_t     size = 0;
    do {
        size = read(peer.fd.get(), request.data(), request.size());
    } while (size < 0 && errno == EINTR);

    if (size <= 0)
        return;

    request.resize(sc<size_t>(size));

    // The reply can be larger than the socket buffer, so it is written blocking. A peer that
    // stops reading is cut off after a second.
    const timeval TIMEOUT = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(peer.fd.get(), SOL_SOCKET, SO_SNDTIMEO, &TIMEOUT, sizeof(TIMEOUT));
    if (const int FLAGS = fcntl(peer.fd.get(), F_GETFL); FLAGS >= 0)
        fcntl(peer.fd.get(), F_SETFL, FLAGS & ~O_NONBLOCK);

    sendAll(peer.fd.get(), dispatch(request));
}

void CAgentIPC::removeRequestPeer(SRequestPeer* peer) {
    if (peer->source)
        wl_event_source_remove(peer->source);

    peer->source = nullptr;
    std::erase_if(m_requestPeers, [peer](const auto& other) { return other.get() == peer; });
}

std::string CAgentIPC::dispatch(const std::string& request) const {
    std::string command = request;
    bool        json    = false;

    // Same flag syntax as the human socket: "j/clients".
    const auto SLASH = command.find('/');
    const auto SPACE = command.find(' ');
    if (SLASH != std::string::npos && (SPACE == std::string::npos || SLASH < SPACE)) {
        json    = command.substr(0, SLASH).contains('j');
        command = command.substr(SLASH + 1);
    }

    while (!command.empty() && (command.back() == '\n' || command.back() == ' '))
        command.pop_back();

    const auto FOCUSED = g_pAgentManager ? g_pAgentManager->focusedWindow(m_actor) : nullptr;

    if (command == "monitors") {
        const auto MONITOR = g_pAgentManager ? g_pAgentManager->monitorFor(m_actor) : nullptr;
        if (!MONITOR)
            return json ? "[]" : "no monitor";

        return monitorData(MONITOR, json);
    }

    if (command == "clients") {
        std::string result = json ? "[" : "";
        bool        first  = true;
        for (const auto& w : controlledWindows(m_actor)) {
            if (json && !first)
                result += ",";
            result += windowData(w, json, w == FOCUSED);
            first = false;
        }

        if (json)
            result += "]";

        return result;
    }

    if (command == "activewindow") {
        if (!FOCUSED || controllerOf(FOCUSED) != m_actor)
            return json ? "{}" : "Invalid";

        return windowData(FOCUSED, json, true);
    }

    if (command.starts_with("dispatch ")) {
        const auto ARGS       = std::string_view{command}.substr(9);
        const auto SEPARATOR  = ARGS.find(' ');
        const auto DISPATCHER = ARGS.substr(0, SEPARATOR);
        const auto VALUE      = SEPARATOR == std::string_view::npos ? std::string_view{} : ARGS.substr(SEPARATOR + 1);

        // Positions are monitor-local, the same coordinates "clients" reports.
        if (DISPATCHER == "movecursor") {
            const auto POSITION = parsePosition(VALUE);
            if (!POSITION)
                return "movecursor takes X Y";

            g_pAgentManager->warpPointer(m_actor, *POSITION);
            return "ok";
        }

        if (DISPATCHER != "focuswindow" && DISPATCHER != "closewindow")
            return std::format("unknown dispatcher {}", DISPATCHER);

        const auto WINDOW = controlledWindow(m_actor, VALUE);
        if (!WINDOW)
            return "no such window";

        if (DISPATCHER == "focuswindow")
            g_pAgentManager->focusWindow(m_actor, WINDOW);
        else
            WINDOW->sendClose();

        return "ok";
    }

    return "unknown request";
}

int CAgentIPC::onEventSocket(int fd, uint32_t mask, void* data) {
    const auto IPC = sc<CAgentIPC*>(data);

    while (true) {
        CFileDescriptor client{accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (!client.isValid())
            break;

        auto peer    = makeUnique<SEventPeer>();
        peer->ipc    = IPC;
        peer->fd     = std::move(client);
        peer->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, peer->fd.get(), 0, onEventPeer, peer.get());

        if (peer->source)
            IPC->m_eventPeers.emplace_back(std::move(peer));
    }

    return 0;
}

int CAgentIPC::onEventPeer(int fd, uint32_t mask, void* data) {
    const auto PEER = sc<SEventPeer*>(data);

    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) || ((mask & WL_EVENT_WRITABLE) && !PEER->ipc->flush(*PEER)))
        PEER->ipc->removeEventPeer(PEER);

    return 0;
}

bool CAgentIPC::flush(SEventPeer& peer) {
    while (!peer.queue.empty()) {
        const auto WRITTEN = send(peer.fd.get(), peer.queue.data(), peer.queue.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

        if (WRITTEN > 0) {
            peer.queue.erase(0, sc<size_t>(WRITTEN));
            continue;
        }

        if (WRITTEN < 0 && errno == EINTR)
            continue;

        if (WRITTEN < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wl_event_source_fd_update(peer.source, WL_EVENT_WRITABLE);
            return true;
        }

        return false;
    }

    wl_event_source_fd_update(peer.source, 0);
    return true;
}

void CAgentIPC::removeEventPeer(SEventPeer* peer) {
    if (peer->source)
        wl_event_source_remove(peer->source);

    peer->source = nullptr;
    std::erase_if(m_eventPeers, [peer](const auto& other) { return other.get() == peer; });
}

void CAgentIPC::postEvent(const std::string& event, const std::string& data) {
    const auto               LINE = std::format("{}>>{}\n", event, data);

    std::vector<SEventPeer*> dead;
    for (const auto& peer : m_eventPeers) {
        if (peer->queue.size() + LINE.size() > MAX_EVENT_QUEUE) {
            dead.emplace_back(peer.get());
            continue;
        }

        peer->queue += LINE;
        if (!flush(*peer))
            dead.emplace_back(peer.get());
    }

    for (const auto& peer : dead) {
        removeEventPeer(peer);
    }
}
