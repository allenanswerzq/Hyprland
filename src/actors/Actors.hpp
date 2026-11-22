#pragma once

#include <optional>
#include <vector>

#include "ActorTypes.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/memory/Memory.hpp"

struct wl_client;
struct wl_global;
class CWLSurfaceResource;
class CWLOutputResource;

/*
    Small ownership helpers used by code outside src/actors.

    Without an actor registry (for example in unit tests) Hyprland has one actor, and every
    object belongs to the human. With a registry, a client without an owner is a bug, and the
    helpers treat it as controlled by nobody.
*/
namespace Actors {
    std::optional<ActorID> ownerOf(const wl_client* client);
    std::optional<ActorID> controllerOf(const wl_client* client);
    std::optional<ActorID> controllerOf(const SP<CWLSurfaceResource>& surface);
    ActorID                controllerOf(const PHLWINDOW& window);
    ActorID                actorOf(const PHLMONITOR& monitor);
    ActorID                actorOf(const PHLWORKSPACE& workspace);

    bool                   isAgent(ActorID id);
    bool                   isAgentMonitor(const PHLMONITOR& monitor);
    bool                   humanControls(const wl_client* client);
    bool                   humanControls(const SP<CWLSurfaceResource>& surface);
    bool                   humanControls(const PHLWINDOW& window);

    // Decides whether a client may see a global. Sandbox rules are applied by the caller.
    bool globalVisibleTo(const wl_client* client, const wl_global* global, bool privileged);

    // wl_output resources of a client for a monitor, including borrow proxies.
    std::vector<SP<CWLOutputResource>> outputResourcesFor(const PHLMONITOR& monitor, wl_client* client);

    /*
        libwayland asks the global filter about a new global inside wl_global_create, before
        Hyprland can record which monitor or client the global is for. Code that creates a
        wl_output global holds one of these while it does, so the filter knows who may see
        the new global. Without a scope, a new wl_output is visible only to the human.
    */
    class CPendingOutputGlobal {
      public:
        // The output belongs to an actor's monitor.
        explicit CPendingOutputGlobal(ActorID actor);
        // The output is a borrow proxy, private to one client.
        explicit CPendingOutputGlobal(const wl_client* onlyClient);
        ~CPendingOutputGlobal();

        CPendingOutputGlobal(const CPendingOutputGlobal&)            = delete;
        CPendingOutputGlobal& operator=(const CPendingOutputGlobal&) = delete;

        struct SScope {
            ActorID          actor  = HUMAN_ACTOR_ID;
            const wl_client* client = nullptr;
        };

      private:
        std::optional<SScope> m_previous;
    };
}
