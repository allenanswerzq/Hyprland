#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ActorTypes.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/math/Math.hpp"
#include "../helpers/memory/Memory.hpp"

struct wl_client;
struct wl_display;
struct wl_global;
class CWLSurfaceResource;
class CWLOutputResource;
class CXDGPopupResource;
class IDataSource;
class IKeyboard;
class IPointer;

namespace Aquamarine {
    class IOutput;
}

namespace Desktop {
    enum eFocusReason : uint32_t;
}

namespace IPC::Socket1 {
    class CSocket1;
}

namespace IPC::Socket2 {
    struct SEvent;
}

/*
    The only header code outside src/actors includes. See src/actors/README.md.

    An actor is one desktop user: the human (ID 0) or an agent (agent-001 is ID 1, and so on).
    Every Wayland client has an owner, the actor whose socket it connected to, and a controller,
    the actor whose devices drive it. They differ only while the human borrows an agent client.

    Every function here works without actors (for example in unit tests, before init() or after
    destroy()): there is then only the human, who owns and controls everything.
*/
namespace Actors {

    // ------------------------------------------------------------------ lifecycle

    // Right after wl_display_create. Every client created from now on gets an owner.
    void init(wl_display* display);
    // After the human socket exists. Reads HYPRLAND_AGENT_COUNT and creates one Wayland socket
    // per agent, named wayland-agent-NNN, next to the human one.
    std::expected<void, std::string> createAgentSockets(const std::string& humanSocket);
    // Once the backend runs: creates the agent monitors, seats and IPC sockets.
    void start();
    // After wl_display_destroy_clients.
    void destroy();

    // hyprctl actors, and hyprctl actor borrow|return <window>.
    void registerCommands(IPC::Socket1::CSocket1& socket);

    // ------------------------------------------------------------------ identity

    // Creates a client owned by owner on fd. Without actors, a plain client. On failure, the
    // caller still owns fd.
    wl_client* createClient(wl_display* display, int fd, ActorID owner);

    // nullopt for a client without an owner, which is a bug while actors exist.
    std::optional<ActorID> ownerOf(const wl_client* client);
    std::optional<ActorID> controllerOf(const wl_client* client);
    std::optional<ActorID> controllerOf(const SP<CWLSurfaceResource>& surface);
    // The controller of the window's client. Once the surface is gone, the window's owner.
    ActorID controllerOf(const PHLWINDOW& window);
    ActorID actorOf(const PHLMONITOR& monitor);
    ActorID actorOf(const PHLWORKSPACE& workspace);
    // The actor whose monitor an output becomes: an agent for a headless output with that
    // agent's reserved name (AGENT-NNN), the human for everything else.
    ActorID actorForOutput(const SP<Aquamarine::IOutput>& output);

    bool    isAgent(ActorID id);
    bool    isAgentMonitor(const PHLMONITOR& monitor);
    bool    humanControls(const wl_client* client);
    bool    humanControls(const SP<CWLSurfaceResource>& surface);
    bool    humanControls(const PHLWINDOW& window);

    // A window the human borrowed from an agent: an agent owns it, and the human controls it.
    bool isBorrowed(const PHLWINDOW& window);
    // The tags a borrowed window carries, for example "borrowed" and "agent-001".
    std::vector<std::string> borrowTags(ActorID owner);

    // "human", "agent-001", ..., or "unknown".
    std::string nameOf(ActorID id);
    // AGENT-NNN output names belong to the agents. hyprctl output create must not use them.
    bool isReservedOutputName(std::string_view name);
    // A named workspace called like an agent ("agent-001") belongs to that agent. True if name
    // is such a workspace and actor is someone else.
    bool isReservedWorkspaceName(std::string_view name, ActorID actor);

    // ------------------------------------------------------------------ desktops

    /*
        Which desktop an object is on. A window is on its controller's desktop; once its
        surface is gone (as on destroy), on the desktop of its workspace. The human's config
        (Lua) and the human socket2 see only the human's desktop.
    */
    ActorID desktopOf(const PHLWINDOW& window);
    ActorID desktopOf(const PHLWORKSPACE& workspace);
    ActorID desktopOf(const PHLMONITOR& monitor);
    ActorID desktopOf(const PHLLS& layer);

    bool    humanSees(const PHLWORKSPACE& workspace);
    bool    humanSees(const PHLWINDOW& window);

    // The agent's monitor, or nullptr for the human or an agent without one.
    PHLMONITOR monitorOf(ActorID agent);
    // Where a surface of client opens if it names no monitor: the monitor of the agent that
    // controls the client, or nullptr for a client the human controls.
    PHLMONITOR agentMonitorOf(const wl_client* client);

    // Agent monitors sit far away from the human layout, so "which monitor is at this point"
    // never mixes human and agent space. Called after the human monitors were arranged.
    void placeAgentMonitors();
    // Where a monitor starts for its clients. An agent's clients see only the agent's monitor,
    // so it starts at 0,0 for them.
    Vector2D outputPosition(const PHLMONITOR& monitor, const Vector2D& position);

    // ------------------------------------------------------------------ events

    // Whose socket2 gets an event about subject: the actor whose desktop it is on.
    ActorID eventAudience(const SEventSubject& subject);
    // Sends an event to an agent's socket2. IPC::Socket2 calls it for events of agents.
    void postAgentEvent(ActorID agent, const IPC::Socket2::SEvent& event);

    // ------------------------------------------------------------------ focus and input

    /*
        Human focus requests for a window the human does not control. Lifecycle reasons (a new
        window, the focused window closing, ...) move the window's agent focus; reasons that
        come from human input (keybinds, clicks, follow mouse) never do.
    */
    // True if the request must be refused.
    bool refuseHumanFocus(const PHLWINDOW& window, Desktop::eFocusReason reason);
    // True if the window belongs to an agent seat. The request went there or was refused.
    bool routeWindowFocus(const PHLWINDOW& window, Desktop::eFocusReason reason, const SP<CWLSurfaceResource>& surface);
    // True if the surface belongs to an agent seat, which then focused it.
    bool routeSurfaceFocus(const SP<CWLSurfaceResource>& surface);
    // An agent window was closed while focused. Focuses the next window of the agent's
    // workspace on its agent seat, and returns it (nullptr if there is none).
    PHLWINDOW refocusAfterClose(const PHLWINDOW& window, const PHLWINDOW& nextInGroup);

    // Whether the window is focused by the actor that controls it.
    bool isFocused(const PHLWINDOW& window);
    // Whether an agent window is active for its agent seat, or nullopt for a human window.
    std::optional<bool> agentWindowActive(const PHLWINDOW& window);

    // The keyboard and focus a client's wl_keyboard and wl_pointer follow: its controller's.
    SP<IKeyboard>          keyboardFor(const wl_client* client);
    SP<CWLSurfaceResource> keyboardFocusFor(const wl_client* client);
    SP<CWLSurfaceResource> pointerFocusFor(const wl_client* client);

    // A virtual keyboard or pointer of an agent client drives that agent's seat. True if it
    // was taken; false for a human device.
    bool adoptVirtualKeyboard(const wl_client* client, const SP<IKeyboard>& keyboard);
    bool adoptVirtualPointer(const wl_client* client, const SP<IPointer>& pointer);

    // Selections and popup grabs of agent clients stay on their agent seat. True if handled.
    bool routeSelection(const wl_client* client, const SP<IDataSource>& source, bool primary);
    bool routePopupGrab(const wl_client* client, const SP<CXDGPopupResource>& popup);

    // ------------------------------------------------------------------ protocol rules

    // Whether a client may see a global. Sandbox rules are applied by the caller.
    bool globalVisibleTo(const wl_client* client, const wl_global* global, bool privileged);
    // Who sees a monitor's wl_output global: the clients the monitor's actor owns. A wl_output
    // global created without an audience is visible only to the human's clients.
    std::function<bool(const wl_client*)> outputAudience(const PHLMONITOR& monitor);
    // Whether a client may capture a monitor's contents. An agent's client may capture only
    // its own actor's monitors, even through a borrow proxy of a human monitor.
    bool mayCapture(const wl_client* client, const PHLMONITOR& monitor);
    // Whether an xdg_activation token from issuer may activate window. Tokens never cross
    // actors: an agent cannot raise a human window, and the other way around.
    bool mayActivate(const wl_client* issuer, const PHLWINDOW& window);

    // wl_output resources of a client for a monitor, including borrow proxies.
    std::vector<SP<CWLOutputResource>> outputResourcesFor(const PHLMONITOR& monitor, wl_client* client);
}
