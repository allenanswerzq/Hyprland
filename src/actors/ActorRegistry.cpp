#include "ActorRegistry.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <hyprutils/os/FileDescriptor.hpp>

#include "../debug/log/Logger.hpp"
#include "../macros.hpp"

using namespace Hyprutils::OS;

namespace Actors {
    class CAgentSocket {
      public:
        static std::expected<UP<CAgentSocket>, std::string> create(CActorRegistry& registry, wl_event_loop* loop, ActorID owner, std::string name, const std::string& runtimeDir);
        ~CAgentSocket();

        CAgentSocket(const CAgentSocket&)            = delete;
        CAgentSocket& operator=(const CAgentSocket&) = delete;

      private:
        CAgentSocket(CActorRegistry& registry, ActorID owner, std::string name, std::string path);

        static int       onReadable(int fd, uint32_t mask, void* data);
        void             acceptClients(uint32_t mask);

        CActorRegistry&  m_registry;
        ActorID          m_owner = HUMAN_ACTOR_ID;
        std::string      m_name;
        std::string      m_path;
        std::string      m_lockPath;
        CFileDescriptor  m_lockFD;
        CFileDescriptor  m_listenFD;
        wl_event_source* m_source      = nullptr;
        bool             m_lockHeld    = false;
        bool             m_socketBound = false;
    };
}

using namespace Actors;

static std::string errnoString() {
    return std::strerror(errno);
}

std::expected<size_t, std::string> Actors::parseAgentCount(std::string_view value) {
    if (value.empty())
        return std::unexpected("agent count is empty");

    size_t     count       = 0;
    const auto BEGIN       = value.data();
    const auto END         = value.data() + value.size();
    const auto [ptr, code] = std::from_chars(BEGIN, END, count);

    if (code != std::errc{} || ptr != END)
        return std::unexpected(std::format("agent count {:?} is not a whole number", value));

    if (count > MAX_AGENT_COUNT)
        return std::unexpected(std::format("agent count {} is above the limit of {}", count, MAX_AGENT_COUNT));

    return count;
}

std::string Actors::agentName(size_t index) {
    return std::format("agent-{:03}", index);
}

std::string Actors::agentSocketName(size_t index) {
    return std::format("wayland-{}", agentName(index));
}

std::string Actors::agentOutputName(size_t index) {
    return std::format("AGENT-{:03}", index);
}

std::string Actors::borrowWorkspaceName(ActorID agent) {
    return std::format("A{}", agent);
}

std::string_view Actors::actorKindToString(eActorKind kind) {
    switch (kind) {
        case eActorKind::HUMAN: return "human";
        case eActorKind::AGENT: return "agent";
    }

    return "unknown";
}

CAgentSocket::CAgentSocket(CActorRegistry& registry, ActorID owner, std::string name, std::string path) :
    m_registry(registry), m_owner(owner), m_name(std::move(name)), m_path(std::move(path)), m_lockPath(m_path + ".lock") {
    ;
}

std::expected<UP<CAgentSocket>, std::string> CAgentSocket::create(CActorRegistry& registry, wl_event_loop* loop, ActorID owner, std::string name, const std::string& runtimeDir) {
    if (!loop)
        return std::unexpected("Wayland event loop is missing");

    if (runtimeDir.empty())
        return std::unexpected("XDG_RUNTIME_DIR is not set");

    auto        socket = UP<CAgentSocket>(new CAgentSocket(registry, owner, name, runtimeDir + "/" + name));

    sockaddr_un address = {};
    if (socket->m_path.size() >= sizeof(address.sun_path))
        return std::unexpected(std::format("socket path {} is too long", socket->m_path));

    socket->m_lockFD = CFileDescriptor{open(socket->m_lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)};
    if (!socket->m_lockFD.isValid())
        return std::unexpected(std::format("cannot open {}: {}", socket->m_lockPath, errnoString()));

    if (flock(socket->m_lockFD.get(), LOCK_EX | LOCK_NB) != 0)
        return std::unexpected(std::format("{} is already in use", socket->m_path));

    socket->m_lockHeld = true;

    struct stat socketStat = {};
    if (lstat(socket->m_path.c_str(), &socketStat) == 0) {
        if (!S_ISSOCK(socketStat.st_mode))
            return std::unexpected(std::format("{} exists and is not a socket", socket->m_path));

        if (unlink(socket->m_path.c_str()) != 0)
            return std::unexpected(std::format("cannot remove stale socket {}: {}", socket->m_path, errnoString()));
    } else if (errno != ENOENT)
        return std::unexpected(std::format("cannot inspect {}: {}", socket->m_path, errnoString()));

    socket->m_listenFD = CFileDescriptor{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!socket->m_listenFD.isValid())
        return std::unexpected(std::format("cannot create socket {}: {}", socket->m_path, errnoString()));

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket->m_path.c_str(), socket->m_path.size() + 1);

    if (bind(socket->m_listenFD.get(), rc<sockaddr*>(&address), sizeof(address)) != 0)
        return std::unexpected(std::format("cannot bind {}: {}", socket->m_path, errnoString()));

    socket->m_socketBound = true;

    if (listen(socket->m_listenFD.get(), 128) != 0)
        return std::unexpected(std::format("cannot listen on {}: {}", socket->m_path, errnoString()));

    socket->m_source = wl_event_loop_add_fd(loop, socket->m_listenFD.get(), WL_EVENT_READABLE, CAgentSocket::onReadable, socket.get());
    if (!socket->m_source)
        return std::unexpected(std::format("cannot watch {}", socket->m_path));

    return socket;
}

CAgentSocket::~CAgentSocket() {
    if (m_source)
        wl_event_source_remove(m_source);

    m_listenFD.reset();

    if (m_socketBound)
        unlink(m_path.c_str());

    if (m_lockHeld)
        unlink(m_lockPath.c_str());
}

int CAgentSocket::onReadable(int fd, uint32_t mask, void* data) {
    sc<CAgentSocket*>(data)->acceptClients(mask);
    return 0;
}

void CAgentSocket::acceptClients(uint32_t mask) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        LOG(Log::ERR, "Agent socket {} reported an error", m_path);
        return;
    }

    if (!(mask & WL_EVENT_READABLE))
        return;

    while (true) {
        CFileDescriptor clientFD{accept4(m_listenFD.get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!clientFD.isValid()) {
            if (errno == EINTR)
                continue;

            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOG(Log::ERR, "Agent socket {} cannot accept a client: {}", m_path, errnoString());

            return;
        }

        const auto CLIENT = m_registry.createClient(clientFD.get(), m_owner);
        if (!CLIENT) {
            LOG(Log::ERR, "Agent socket {} cannot create a Wayland client", m_path);
            continue;
        }

        clientFD.take(); // wl_client owns the fd now.
        LOG(Log::DEBUG, "Agent socket {} accepted Wayland client {:x}", m_name, rc<uintptr_t>(CLIENT));
    }
}

CActorRegistry::CActorRegistry(wl_display* display) : m_display(display) {
    m_actors.push_back({
        .id            = HUMAN_ACTOR_ID,
        .kind          = eActorKind::HUMAN,
        .name          = "human",
        .waylandSocket = "",
    });

    if (!m_display)
        return;

    wl_list_init(&m_clientCreated.listener.link);
    m_clientCreated.listener.notify = CActorRegistry::onClientCreated;
    m_clientCreated.registry        = this;
    wl_display_add_client_created_listener(m_display, &m_clientCreated.listener);
}

CActorRegistry::~CActorRegistry() {
    m_agentSockets.clear();

    for (auto& [client, listener] : m_clientListeners) {
        wl_list_remove(&listener->listener.link);
        wl_list_init(&listener->listener.link);
    }

    m_clientListeners.clear();
    m_clientOwners.clear();

    if (m_display) {
        wl_list_remove(&m_clientCreated.listener.link);
        wl_list_init(&m_clientCreated.listener.link);
    }
}

void CActorRegistry::setHumanSocket(std::string socketName) {
    m_actors.at(HUMAN_ACTOR_ID).waylandSocket = std::move(socketName);
}

std::expected<void, std::string> CActorRegistry::createAgentSockets(size_t count, const std::string& runtimeDir) {
    if (count > MAX_AGENT_COUNT)
        return std::unexpected(std::format("agent count {} is above the limit of {}", count, MAX_AGENT_COUNT));

    if (!m_agentSockets.empty() || m_actors.size() != 1)
        return std::unexpected("agent sockets were already created");

    if (count == 0)
        return {};

    if (!m_display)
        return std::unexpected("Wayland display is missing");

    std::vector<SActor>           actors;
    std::vector<UP<CAgentSocket>> sockets;
    actors.reserve(count);
    sockets.reserve(count);

    for (size_t index = 1; index <= count; ++index) {
        const auto ID    = sc<ActorID>(m_actors.size() + actors.size());
        auto       actor = SActor{
            .id            = ID,
            .kind          = eActorKind::AGENT,
            .name          = agentName(index),
            .waylandSocket = agentSocketName(index),
            .outputName    = agentOutputName(index),
        };

        auto socket = CAgentSocket::create(*this, wl_display_get_event_loop(m_display), ID, actor.waylandSocket, runtimeDir);
        if (!socket)
            return std::unexpected(std::format("cannot create {}: {}", actor.waylandSocket, socket.error()));

        actors.emplace_back(std::move(actor));
        sockets.emplace_back(std::move(*socket));
    }

    for (auto& actor : actors) {
        LOG(Log::DEBUG, "Created actor {} with Wayland socket {}", actor.name, actor.waylandSocket);
        m_actors.emplace_back(std::move(actor));
    }

    for (auto& socket : sockets) {
        m_agentSockets.emplace_back(std::move(socket));
    }

    return {};
}

wl_client* CActorRegistry::createClient(int fd, ActorID owner) {
    if (!m_display || fd < 0 || !actor(owner))
        return nullptr;

    m_pendingOwner    = owner;
    const auto CLIENT = wl_client_create(m_display, fd);
    m_pendingOwner.reset();

    if (!CLIENT)
        return nullptr;

    RASSERT(ownerOf(CLIENT) == owner, "Wayland client {:x} did not receive actor {}", rc<uintptr_t>(CLIENT), owner);

    return CLIENT;
}

std::optional<ActorID> CActorRegistry::ownerOf(const wl_client* client) const {
    const auto IT = m_clientOwners.find(client);
    if (IT == m_clientOwners.end())
        return std::nullopt;

    return IT->second;
}

std::optional<ActorID> CActorRegistry::controllerOf(const wl_client* client) const {
    const auto OWNER = ownerOf(client);
    if (!OWNER)
        return std::nullopt;

    const auto IT = m_clientControllers.find(client);
    return IT == m_clientControllers.end() ? *OWNER : IT->second;
}

bool CActorRegistry::setController(wl_client* client, ActorID controller) {
    const auto OWNER = ownerOf(client);
    if (!OWNER || !actor(controller))
        return false;

    if (controller == *OWNER)
        m_clientControllers.erase(client);
    else
        m_clientControllers[client] = controller;

    return true;
}

std::vector<wl_client*> CActorRegistry::clientsOwnedBy(ActorID owner) const {
    std::vector<wl_client*> clients;
    for (const auto& [client, clientOwner] : m_clientOwners) {
        if (clientOwner == owner)
            clients.emplace_back(const_cast<wl_client*>(client));
    }

    return clients;
}

const SActor* CActorRegistry::actor(ActorID id) const {
    if (id >= m_actors.size() || m_actors[id].id != id)
        return nullptr;

    return &m_actors[id];
}

const SActor* CActorRegistry::actorByName(std::string_view name) const {
    for (const auto& actor : m_actors) {
        if (actor.name == name)
            return &actor;
    }

    return nullptr;
}

std::optional<ActorID> CActorRegistry::actorForOutputName(std::string_view outputName) const {
    for (const auto& actor : m_actors) {
        if (actor.kind == eActorKind::AGENT && actor.outputName == outputName)
            return actor.id;
    }

    return std::nullopt;
}

bool CActorRegistry::isReservedOutputName(std::string_view outputName) const {
    return outputName.starts_with("AGENT-") || actorForOutputName(outputName).has_value();
}

const std::vector<SActor>& CActorRegistry::actors() const {
    return m_actors;
}

size_t CActorRegistry::clientCount(ActorID id) const {
    size_t count = 0;
    for (const auto& [client, owner] : m_clientOwners) {
        if (owner == id)
            ++count;
    }

    return count;
}

bool CActorRegistry::isAgent(ActorID id) const {
    const auto ACTOR = actor(id);
    return ACTOR && ACTOR->kind == eActorKind::AGENT;
}

void CActorRegistry::onClientCreated(wl_listener* listener, void* data) {
    SCreatedListener* wrapper = wl_container_of(listener, wrapper, listener);
    wrapper->registry->assignClient(sc<wl_client*>(data));
}

void CActorRegistry::onClientDestroyed(wl_listener* listener, void* data) {
    SClientListener* wrapper  = wl_container_of(listener, wrapper, listener);
    const auto       REGISTRY = wrapper->registry;
    const auto       CLIENT   = wrapper->client;

    REGISTRY->m_events.clientDestroyed.emit(CLIENT);
    // This frees wrapper.
    REGISTRY->forgetClient(CLIENT);
}

void CActorRegistry::assignClient(wl_client* client) {
    const auto OWNER = m_pendingOwner.value_or(HUMAN_ACTOR_ID);

    auto       listener = makeUnique<SClientListener>();
    wl_list_init(&listener->listener.link);
    listener->listener.notify = CActorRegistry::onClientDestroyed;
    listener->registry        = this;
    listener->client          = client;

    wl_client_add_destroy_late_listener(client, &listener->listener);
    m_clientOwners[client]    = OWNER;
    m_clientListeners[client] = std::move(listener);

    const auto ACTOR = actor(OWNER);
    LOG(Log::DEBUG, "Wayland client {:x} belongs to actor {}", rc<uintptr_t>(client), ACTOR ? ACTOR->name : std::to_string(OWNER));
}

void CActorRegistry::forgetClient(wl_client* client) {
    m_clientOwners.erase(client);
    m_clientControllers.erase(client);
    m_clientListeners.erase(client);
}
