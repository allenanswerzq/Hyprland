#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <wayland-server-core.h>

#include "ActorTypes.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

namespace Actors {
    enum class eActorKind : uint8_t {
        HUMAN = 0,
        AGENT,
    };

    struct SActor {
        ActorID     id   = HUMAN_ACTOR_ID;
        eActorKind  kind = eActorKind::HUMAN;
        std::string name;
        std::string waylandSocket;
        std::string outputName; // reserved headless output name, agents only
    };

    std::expected<size_t, std::string> parseAgentCount(std::string_view value);
    std::string                        agentName(size_t index);
    std::string                        agentSocketName(size_t index);
    std::string                        agentOutputName(size_t index);
    std::string_view                   actorKindToString(eActorKind kind);

    class CAgentSocket;

    class CActorRegistry {
      public:
        explicit CActorRegistry(wl_display* display);
        ~CActorRegistry();

        CActorRegistry(const CActorRegistry&)                        = delete;
        CActorRegistry& operator=(const CActorRegistry&)             = delete;
        CActorRegistry(CActorRegistry&&)                             = delete;
        CActorRegistry&                  operator=(CActorRegistry&&) = delete;

        void                             setHumanSocket(std::string socketName);
        std::expected<void, std::string> createAgentSockets(size_t count, const std::string& runtimeDir);

        // Creates a client with a known owner. On failure, the caller still owns fd.
        wl_client*             createClient(int fd, ActorID owner);

        std::optional<ActorID> ownerOf(const wl_client* client) const;
        // The controller is the actor whose devices drive the client. It equals the owner
        // unless the human borrowed the client.
        std::optional<ActorID>     controllerOf(const wl_client* client) const;
        bool                       setController(wl_client* client, ActorID controller);
        std::vector<wl_client*>    clientsOwnedBy(ActorID owner) const;

        const SActor*              actor(ActorID id) const;
        const SActor*              actorByName(std::string_view name) const;
        std::optional<ActorID>     actorForOutputName(std::string_view outputName) const;
        bool                       isReservedOutputName(std::string_view outputName) const;
        const std::vector<SActor>& actors() const;
        size_t                     clientCount(ActorID id) const;
        bool                       isAgent(ActorID id) const;

        struct {
            // Emitted after libwayland destroyed all resources of the client, just before
            // the registry forgets it. Owner and controller are still known here.
            CSignalT<wl_client*> clientDestroyed;
        } m_events;

      private:
        struct SCreatedListener {
            wl_listener     listener = {};
            CActorRegistry* registry = nullptr;
        };

        struct SClientListener {
            wl_listener     listener = {};
            CActorRegistry* registry = nullptr;
            wl_client*      client   = nullptr;
        };

        static void                                               onClientCreated(wl_listener* listener, void* data);
        static void                                               onClientDestroyed(wl_listener* listener, void* data);

        void                                                      assignClient(wl_client* client);
        void                                                      forgetClient(wl_client* client);

        wl_display*                                               m_display = nullptr;
        SCreatedListener                                          m_clientCreated;
        std::optional<ActorID>                                    m_pendingOwner;
        std::vector<SActor>                                       m_actors;
        std::unordered_map<const wl_client*, ActorID>             m_clientOwners;
        std::unordered_map<const wl_client*, ActorID>             m_clientControllers;
        std::unordered_map<const wl_client*, UP<SClientListener>> m_clientListeners;
        std::vector<UP<CAgentSocket>>                             m_agentSockets;
    };
}

inline UP<Actors::CActorRegistry> g_pActorRegistry;
