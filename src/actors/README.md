# Actors

An actor is one user of the desktop. The human is actor 0. Agents are actors 1 and up,
named `agent-001`, `agent-002`, and so on. Each actor has its own Wayland socket
(`wayland-1` for the human, `wayland-agent-001` for agent 1), and every Wayland client
belongs to the actor whose socket it connected to.

The human owns the physical keyboard, mouse and screens. Each agent gets a headless
monitor (`AGENT-001`), a seat driven by its own virtual keyboard and pointer, and its own
IPC sockets. Agents cannot see the human's screens or devices, or each other's.

## Owner and controller

Every client has two actors:

- The **owner** is the actor whose socket the client connected to. It never changes.
  It decides which outputs and protocols the client can see.
- The **controller** is the actor whose devices drive the client. It equals the owner,
  except while the human borrows an agent's client (`hyprctl actor borrow`). A borrowed
  client gets human input and its windows live on a human workspace (`A1` for agent 1).

## Layers

Code outside this folder includes only `Actors.hpp`, and headers that need the `ActorID`
type include the small `ActorTypes.hpp`. The free functions work without actors too (in
unit tests, or before `init`): there is then only the human.

| File | Role |
|---|---|
| `Actors.hpp` | The facade: lifecycle, identity, desktops, events, focus, input and protocol rules. |
| `Actors.cpp` | Lifecycle, identity, desktops and event routing. |
| `ActorPolicy.cpp` | The rules that keep actors apart: focus, input, selections, globals, capture, activation. |
| `ActorCommands.cpp` | `hyprctl actors`, `hyprctl actor borrow` and `hyprctl actor return` on the human IPC socket. |
| `ActorRegistry` | Knows the actors, their Wayland sockets, and the owner and controller of each client. |
| `AgentManager` | Creates agent monitors, seats and IPC at startup, and runs borrowing. |
| `AgentSeat` | One agent's keyboard focus, pointer, clipboard and popup grabs. |
| `AgentIPC` | One agent's `.socket.sock` and `.socket2.sock`, built from the human socket classes. |

The two globals, `g_pActorRegistry` and `g_pAgentManager`, are private to this folder.

## Startup

1. `CCompositor::initServer` calls `Actors::init` right after `wl_display_create`, so every
   client gets an owner.
2. After the human socket exists, `Actors::createAgentSockets` reads `HYPRLAND_AGENT_COUNT`
   and adds one socket per agent.
3. `CCompositor::startCompositor` calls `Actors::start` once the backend runs. It creates
   the agent monitors, seats and IPC sockets.
4. `CCompositor::cleanup` calls `Actors::destroy` after all clients are gone.

## Monitors

`State::monitorState()` keeps agent monitors apart from the human's:

- `allMonitors()` and `monitors()` hold only human monitors. Everything that is about
  the human's screens loops over them unchanged: layout, pointer, cursor, fallback output,
  session lock, DPMS, refresh rate.
- `agentMonitors()` holds every agent monitor, enabled or not.
- `renderMonitors()` holds `monitors()` and the enabled agent monitors. Code that follows
  a surface wherever it is drawn uses it: damage, frame pacing, animations, decorations.

A new loop over `monitors()` therefore never meets an agent monitor by surprise. Monitor
queries (`State::monitorState()->query()`) search both lists, filtered by actor.

Agent monitors still fire the shared `monitor.added` and `monitor.removed` events, because
the protocols need them (`wl_output`, DMA-BUF feedback, presentation time). Listeners that
are about the human's screens check `Actors::isAgentMonitor`.

## Windows and layers

Hyprland's window code calls a few hooks:

- `CWindow::mapWindow` opens an agent window on its agent's monitor
  (`Actors::monitorOf`) and skips the rules that move windows around the human layout.
- `CWindow::unmapWindow` lets `Actors::refocusAfterClose` focus the agent's next window on
  its agent seat.
- `CLayerSurface::create` opens an agent's layer surface that names no monitor on the
  agent's monitor (`Actors::agentMonitorOf`).

`CAgentManager` tags a new window of a borrowed client `borrowed` and `agent-NNN` when it
is created (`window.create`), so window rules see the tags when they are read at map.

## Seats

`CWLSeatProtocol` asks `Actors::keyboardFor`, `keyboardFocusFor` and `pointerFocusFor`
which devices and focus a client follows, and offers each client its controller's
capabilities (`capsForClient`). Everything else stays here: `CAgentSeat` sends an agent's
keymap to the clients it controls, and `CAgentManager` refreshes a client's seat when a
borrow changes its controller.

## Events

Every socket2 event may carry a subject (`IPC::Socket2::SEvent::subject`): the window,
workspace, monitor or layer it is about, or an actor directly. `IPC::Socket2::CSocket2::postEvent`
sends the event to the socket2 of the actor whose desktop the subject is on. Without a
subject the event goes to the human.

A window is on its controller's desktop, so events about a borrowed window go to the
human, like events about any human window.

Some events are about an actor rather than an object, and name it directly: an agent
seat's focus changes and keyboard layouts (`activewindow`, `activelayout`), and a
capture by an agent's client (`screencast`).

Lua config events follow the same rule (`Actors::eventAudience`): the config is the human's,
so `CLuaEventHandler::dispatch` drops an event whose subject is on an agent's desktop. Lua
window and workspace selectors resolve only to objects on the human desktop.

The human's `hyprctl` requests (`clients`, `workspaces`, `monitors`) are an admin view
and list everything. An agent's `hyprctl` sees only that agent's desktop.

## Outputs

A `wl_output` global is created with an audience (`Actors::outputAudience`): only clients
owned by the monitor's actor see it. The compositor's global filter checks the audience
through `IWaylandProtocol::fromGlobal`. A `wl_output` global without an audience is
visible to human clients only.

A borrowed client still belongs to its agent, so it cannot see the human monitors. While
borrowed, it gets one proxy `wl_output` per human monitor whose audience is that client
alone. The proxies go away when the client is returned.
