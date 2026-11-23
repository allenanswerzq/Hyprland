#pragma once

#include <expected>
#include <string>
#include <vector>

#include "../desktop/DesktopTypes.hpp"
#include "../helpers/math/Math.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

namespace Actors {
    /*
        Shows one window inside another: Hyprland draws the embedded window's surfaces in a box
        of its host window, from the same GPU buffers, and paces and scales the embedded window
        as if it were on the host's monitor.

        Parallel Chrome uses it to show a page of the human's Chrome, in a hidden window, inside
        the agent's Chrome window. Input is not part of it: the host's client sends the embedded
        window whatever input it should get, by its own means.

        An embedded window is not drawn anywhere else unless its own workspace is shown. A window
        can be embedded in one host at a time, and a host cannot itself be embedded.
    */
    class CWindowEmbeds {
      public:
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

        CWindowEmbeds();

        CWindowEmbeds(const CWindowEmbeds&)            = delete;
        CWindowEmbeds& operator=(const CWindowEmbeds&) = delete;

        // Embeds window in host, or moves it there if it is embedded elsewhere.
        std::expected<void, std::string> embed(PHLWINDOW window, PHLWINDOW host, const CBox& box, const Vector2D& source);
        std::expected<void, std::string> unembed(PHLWINDOW window);

        // The windows embedded in host, in the order they were embedded.
        std::vector<SEmbed> embeddedIn(const PHLWINDOW& host) const;
        // The host window shows window in, or nullptr.
        PHLWINDOW hostOf(const PHLWINDOW& window) const;

        // Damages the host's box that shows window, after window changed.
        void damage(const PHLWINDOW& window) const;

      private:
        const SEmbed*       embedOf(const PHLWINDOW& window) const;
        void                damageBox(const SEmbed& embed) const;
        void                onWindowClosed(const PHLWINDOW& window);

        std::vector<SEmbed> m_embeds;
        CHyprSignalListener m_windowClose;
    };
}

inline UP<Actors::CWindowEmbeds> g_pWindowEmbeds;
