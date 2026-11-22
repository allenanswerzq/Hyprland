#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <hyprutils/os/FileDescriptor.hpp>

#include "ActorTypes.hpp"
#include "../helpers/memory/Memory.hpp"

struct wl_event_source;

namespace Actors {
    /*
        The IPC sockets of one agent's desktop.

        The directory is {instance path}-agent-NNN and holds .socket.sock and .socket2.sock,
        the same layout as the human instance. Stock hyprctl works with
        HYPRLAND_INSTANCE_SIGNATURE set to {signature}-agent-NNN.

        Every request only sees and acts on windows the agent controls:

         - clients, activewindow (the j/ prefix selects JSON)
         - dispatch focuswindow address:0x...
         - dispatch closewindow address:0x...

        socket2 sends openwindow, closewindow, activewindow and activewindowv2.
    */
    class CAgentIPC {
      public:
        static std::expected<UP<CAgentIPC>, std::string> create(ActorID actor, std::string directory);
        ~CAgentIPC();

        CAgentIPC(const CAgentIPC&)            = delete;
        CAgentIPC& operator=(const CAgentIPC&) = delete;

        void       postEvent(const std::string& event, const std::string& data);

      private:
        CAgentIPC(ActorID actor, std::string directory);

        // A hyprctl connection waiting for its request.
        struct SRequestPeer {
            CAgentIPC*                     ipc = nullptr;
            Hyprutils::OS::CFileDescriptor fd;
            wl_event_source*               source = nullptr;
        };

        // A socket2 listener.
        struct SEventPeer {
            CAgentIPC*                     ipc = nullptr;
            Hyprutils::OS::CFileDescriptor fd;
            wl_event_source*               source = nullptr;
            std::string                    queue;
        };

        std::expected<void, std::string> listen();
        static int                       onRequestSocket(int fd, uint32_t mask, void* data);
        static int                       onRequestPeer(int fd, uint32_t mask, void* data);
        static int                       onEventSocket(int fd, uint32_t mask, void* data);
        static int                       onEventPeer(int fd, uint32_t mask, void* data);

        void                             handleRequest(SRequestPeer& peer);
        std::string                      dispatch(const std::string& request) const;
        bool                             flush(SEventPeer& peer);
        void                             removeRequestPeer(SRequestPeer* peer);
        void                             removeEventPeer(SEventPeer* peer);

        ActorID                          m_actor = HUMAN_ACTOR_ID;
        std::string                      m_directory;
        std::string                      m_requestPath;
        std::string                      m_eventPath;
        bool                             m_createdDirectory = false;

        Hyprutils::OS::CFileDescriptor   m_requestSocket;
        Hyprutils::OS::CFileDescriptor   m_eventSocket;
        wl_event_source*                 m_requestSource = nullptr;
        wl_event_source*                 m_eventSource   = nullptr;
        std::vector<UP<SRequestPeer>>    m_requestPeers;
        std::vector<UP<SEventPeer>>      m_eventPeers;
    };
}
