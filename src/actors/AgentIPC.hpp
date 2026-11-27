#pragma once

#include <expected>
#include <string>

#include "ActorTypes.hpp"
#include "../helpers/memory/Memory.hpp"

namespace IPC::Socket1 {
    class CSocket1;
}

namespace IPC::Socket2 {
    class CSocket2;
    struct SEvent;
}

namespace Actors {
    /*
        The IPC sockets of one agent's desktop, built from the same socket classes as the
        human's.

        The directory is {instance path}-agent-NNN and holds .socket.sock and .socket2.sock,
        the same layout as the human instance, so stock hyprctl works with
        HYPRLAND_INSTANCE_SIGNATURE set to {signature}-agent-NNN.

        Requests see and act on the agent's own desktop only. Positions are relative to the
        agent's monitor, which starts at 0,0:

         - monitors, clients, activewindow (the j/ prefix selects JSON)
         - dispatch focuswindow address:0x...
         - dispatch closewindow address:0x...
         - dispatch movecursor X Y

        socket2 gets every event whose subject is on the agent's desktop (see
        IPC::Socket2::SEvent::subject), with the same format as the human's socket2.
    */
    class CAgentIPC {
      public:
        static std::expected<UP<CAgentIPC>, std::string> create(ActorID actor, std::string directory);
        ~CAgentIPC();

        CAgentIPC(const CAgentIPC&)            = delete;
        CAgentIPC& operator=(const CAgentIPC&) = delete;

        void       postEvent(const IPC::Socket2::SEvent& event);

      private:
        CAgentIPC(ActorID actor, std::string directory);

        void                       registerCommands();

        ActorID                    m_actor = HUMAN_ACTOR_ID;
        std::string                m_directory;
        bool                       m_createdDirectory = false;

        UP<IPC::Socket1::CSocket1> m_requests;
        UP<IPC::Socket2::CSocket2> m_events;
    };
}
