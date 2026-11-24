#pragma once

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ActorTypes.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/math/Math.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

struct wl_client;
class CWLOutputResource;
class CWLOutputProtocol;

namespace IPC::Socket2 {
    struct SEvent;
}

namespace Actors {
    class CAgentSeat;
    class CAgentIPC;

    /*
        Runs everything agents have that the human seat code does not know about:

         - one headless monitor per agent (AGENT-NNN), created at startup,
         - one agent seat per agent, driven by the agent's virtual devices,
         - one IPC directory per agent, so stock hyprctl can talk to the agent's desktop,
         - borrowing: the human can take over one agent client, and give it back later.

        While a client is borrowed, the human controls it. Its windows live on a human
        workspace and get human input, exactly like human windows. Borrowing puts the client's
        windows together on the agent's borrow workspace (A1 for agent-001); after that they move
        like any human window, and new ones open where the human is. The client still belongs to
        the agent (its owner does not change), so it still cannot see human outputs. It gets
        private proxy wl_output globals for the human monitors instead, whose audience is that
        one client.
    */
    class CAgentManager {
      public:
        CAgentManager();
        ~CAgentManager();

        CAgentManager(const CAgentManager&)            = delete;
        CAgentManager& operator=(const CAgentManager&) = delete;

        // Creates the agent monitors and IPC sockets. Called once, after the backend started.
        void start();

        // The agent's seat, or nullptr if the actor is not an agent.
        CAgentSeat*                      seat(ActorID actor) const;
        PHLMONITOR                       monitorFor(ActorID actor) const;
        PHLWORKSPACE                     workspaceFor(ActorID actor) const;

        std::expected<void, std::string> borrow(PHLWINDOW window);
        std::expected<void, std::string> returnWindow(PHLWINDOW window);
        bool                             isBorrowed(const wl_client* client) const;

        // The human workspace that holds the windows borrowed from an agent, for example A1.
        // With create, a missing one is made on the human's focused monitor.
        PHLWORKSPACE                       borrowWorkspace(ActorID agent, bool create);

        std::vector<SP<CWLOutputResource>> proxyOutputResources(const PHLMONITOR& monitor, wl_client* client) const;

        // Sends an event to the agent's socket2.
        void postEvent(ActorID actor, const IPC::Socket2::SEvent& event);

      private:
        struct SProxyOutput {
            SP<CWLOutputProtocol> output;
            PHLMONITORREF         monitor;
        };

        struct SBorrow {
            wl_client*                client = nullptr;
            ActorID                   owner  = HUMAN_ACTOR_ID;
            std::vector<SProxyOutput> outputs;
        };

        SBorrow*                                    borrowOf(const wl_client* client);
        std::vector<PHLWINDOW>                      windowsOf(const wl_client* client) const;
        void                                        moveClientWindows(const wl_client* client, const PHLWORKSPACE& workspace);
        void                                        addProxyOutput(SBorrow& borrow, const PHLMONITOR& monitor);
        void                                        removeProxyOutputs(SBorrow& borrow, const PHLMONITOR& onlyMonitor = nullptr);

        void                                        onMonitorAdded(const PHLMONITOR& monitor);
        void                                        onMonitorRemoved(const PHLMONITOR& monitor);
        void                                        onClientDestroyed(wl_client* client);

        std::unordered_map<ActorID, UP<CAgentSeat>> m_seats;
        std::unordered_map<ActorID, UP<CAgentIPC>>  m_ipc;
        std::vector<SBorrow>                        m_borrows;
        bool                                        m_started = false;

        struct {
            CHyprSignalListener monitorAdded;
            CHyprSignalListener monitorRemoved;
            CHyprSignalListener clientDestroyed;
            CHyprSignalListener windowCreated;
        } m_listeners;
    };
}

inline UP<Actors::CAgentManager> g_pAgentManager;
