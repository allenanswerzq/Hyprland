#pragma once

#include <expected>
#include <string>
#include <vector>

#include "../desktop/DesktopTypes.hpp"
#include "../helpers/math/Math.hpp"

namespace IPC::Socket1 {
    class CSocket1;
}

/*
    Window embedding: Hyprland draws one window (the embedded window) inside a box of another
    window (the host), from the same GPU buffers. The embedded window is paced and scaled as if
    it were on the host's monitor.

    Parallel Chrome uses it to show a page of the human's Chrome, kept in a hidden window,
    inside the agent's Chrome window. Input is not part of it: the host's client forwards
    whatever input the embedded window should get, by its own means.

    An embedded window is not drawn anywhere else unless its own workspace is shown. A window
    can be embedded in one host at a time, and a host cannot itself be embedded.

    Every function is safe to call before init() or after destroy(): there are no embeds then.
*/
namespace Embed {
    struct SEmbed {
        PHLWINDOWREF window;
        PHLWINDOWREF host;
        // Where the embedded window shows, in the host's logical coordinates, relative to the
        // host's top left corner. Only this box of the host shows it.
        CBox box;
        // The point of the embedded window, in its own logical coordinates, that lands at the
        // box's top left corner.
        Vector2D source;
    };

    void init();
    void destroy();

    // Embeds window in host, or moves it there if it is embedded elsewhere.
    std::expected<void, std::string> embed(PHLWINDOW window, PHLWINDOW host, const CBox& box, const Vector2D& source);
    std::expected<void, std::string> unembed(PHLWINDOW window);

    // The windows embedded in host, in the order they were embedded.
    std::vector<SEmbed> embeddedIn(const PHLWINDOW& host);
    bool                isHost(const PHLWINDOW& window);
    // The host window shows window in, or nullptr.
    PHLWINDOW hostOf(const PHLWINDOW& window);

    // Damages the host's box that shows window, after window changed. Returns false and does
    // nothing if window is not embedded.
    bool damageThroughHost(const PHLWINDOW& window);

    // hyprctl embed <window> <host> <x> <y> <width> <height> <sourceX> <sourceY>, and
    // hyprctl unembed <window>.
    void registerCommands(IPC::Socket1::CSocket1& socket);
}
