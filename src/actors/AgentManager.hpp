#pragma once

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ActorTypes.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

struct wl_client;
struct wl_global;
class IKeyboard;
class IPointer;
class IDataSource;
class CWLSurfaceResource;
class CWLOutputResource;
class CWLOutputProtocol;
class CXDGPopupResource;

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
        workspace and get human input, exactly like human windows. The client still belongs
        to the agent (its owner does not change), so it still cannot see human outputs. It gets
        private proxy wl_output globals for the human monitors instead.
    */
    class CAgentManager {
      public:
        CAgentManager();
        ~CAgentManager();

        CAgentManager(const CAgentManager&)            = delete;
        CAgentManager& operator=(const CAgentManager&) = delete;

        // Creates the agent monitors and IPC sockets. Called once, after the backend started.
        void                             start();

        PHLMONITOR                       monitorFor(ActorID actor) const;
        PHLWORKSPACE                     workspaceFor(ActorID actor) const;

        void                             addVirtualKeyboard(ActorID actor, SP<IKeyboard> keyboard);
        void                             addVirtualPointer(ActorID actor, SP<IPointer> pointer);
        SP<IKeyboard>                    keyboardFor(ActorID actor) const;
        SP<CWLSurfaceResource>           keyboardFocus(ActorID actor) const;
        SP<CWLSurfaceResource>           pointerFocus(ActorID actor) const;

        void                             focusWindow(ActorID actor, PHLWINDOW window, SP<CWLSurfaceResource> surface = nullptr);
        void                             focusSurface(ActorID actor, SP<CWLSurfaceResource> surface);
        PHLWINDOW                        focusedWindow(ActorID actor) const;
        bool                             isWindowActive(const PHLWINDOW& window) const;

        void                             setSelection(ActorID actor, SP<IDataSource> source, bool primary);
        void                             addPopupGrab(ActorID actor, SP<CXDGPopupResource> popup);

        std::expected<void, std::string> borrow(PHLWINDOW window);
        std::expected<void, std::string> returnWindow(PHLWINDOW window);
        bool                             isBorrowed(const wl_client* client) const;

        // The client a borrow proxy output global belongs to, or nullopt if the global is not
        // a borrow proxy.
        std::optional<const wl_client*>    proxyOutputClient(const wl_global* global) const;
        std::vector<SP<CWLOutputResource>> proxyOutputResources(const PHLMONITOR& monitor, wl_client* client) const;

        // Posts an event to the agent's socket2.
        void postEvent(ActorID actor, const std::string& event, const std::string& data);

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

        // A proxy whose global is gone but whose wl_output resources are still bound. The
        // global filter still has to know who it belongs to until libwayland forgets it.
        struct SRetiredProxy {
            WP<CWLOutputProtocol> output;
            const wl_client*      client = nullptr;
        };

        CAgentSeat*                                 seat(ActorID actor) const;
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
        std::vector<SRetiredProxy>                  m_retiredProxies;
        bool                                        m_started = false;

        struct {
            CHyprSignalListener monitorAdded;
            CHyprSignalListener monitorRemoved;
            CHyprSignalListener windowOpen;
            CHyprSignalListener windowClose;
            CHyprSignalListener clientDestroyed;
        } m_listeners;
    };
}

inline UP<Actors::CAgentManager> g_pAgentManager;
