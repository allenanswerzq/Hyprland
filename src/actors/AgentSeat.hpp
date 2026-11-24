#pragma once

#include <cstdint>
#include <vector>

#include "ActorTypes.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../devices/IKeyboard.hpp"
#include "../devices/IPointer.hpp"
#include "../helpers/math/Math.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

struct wl_client;
class CWLSurfaceResource;
class CXDGPopupResource;
class CSeatGrab;
class IDataSource;

namespace Actors {
    /*
        The input state of one agent.

        The human seat lives in CSeatManager and is driven by physical devices. Each agent has
        a seat of its own that is driven only by the virtual keyboards and pointers its clients
        create. The two never share focus, pointer position, popup grabs or clipboard.

        An agent seat sends events only to clients the agent controls, and only to surfaces on
        the agent's own workspace.
    */
    class CAgentSeat {
      public:
        explicit CAgentSeat(ActorID actor);
        ~CAgentSeat();

        CAgentSeat(const CAgentSeat&)            = delete;
        CAgentSeat& operator=(const CAgentSeat&) = delete;

        ActorID     actor() const;

        void        addKeyboard(SP<IKeyboard> keyboard);
        void        addPointer(SP<IPointer> pointer);

        // The keyboard whose keymap the agent's clients use: the one used last.
        SP<IKeyboard>          keyboard() const;

        void                   focusWindow(PHLWINDOW window, SP<CWLSurfaceResource> surface = nullptr);
        void                   focusSurface(SP<CWLSurfaceResource> surface);
        PHLWINDOW              focusedWindow() const;
        SP<CWLSurfaceResource> keyboardFocus() const;
        SP<CWLSurfaceResource> pointerFocus() const;
        bool                   isWindowActive(const PHLWINDOW& window) const;

        void                   setSelection(SP<IDataSource> source, bool primary);
        void                   addPopupGrab(SP<CXDGPopupResource> popup);

        // Moves the pointer to a position local to the agent's monitor.
        void warpPointer(const Vector2D& local);

        // Drops every piece of state that points at a client: focus, popup grabs and the
        // implicit pointer grab. Used before the client changes controller.
        void releaseClient(const wl_client* client);

        // Called when the agent's monitor appears or changes size.
        void onMonitorChanged();

      private:
        struct SKeyboard {
            SP<IKeyboard>       device;
            CHyprSignalListener key;
            CHyprSignalListener modifiers;
            CHyprSignalListener keymap;
            CHyprSignalListener destroy;
        };

        struct SPointer {
            SP<IPointer>        device;
            CHyprSignalListener motion;
            CHyprSignalListener motionAbsolute;
            CHyprSignalListener button;
            CHyprSignalListener axis;
            CHyprSignalListener frame;
            CHyprSignalListener destroy;
        };

        struct SSelection {
            WP<IDataSource>     source;
            CHyprSignalListener destroy;
        };

        void                               onKey(SP<IKeyboard> keyboard, const IKeyboard::SKeyEvent& event);
        void                               onModifiers(SP<IKeyboard> keyboard, const IKeyboard::SModifiersEvent& event);
        void                               onKeymap(SP<IKeyboard> keyboard);
        void                               useKeyboard(SP<IKeyboard> keyboard);
        void                               sendKeymap(const SP<IKeyboard>& keyboard) const;

        void                               onMotion(uint32_t timeMs, const Vector2D& delta);
        void                               onMotionAbsolute(uint32_t timeMs, const Vector2D& absolute);
        void                               onButton(const IPointer::SButtonEvent& event);
        void                               onAxis(const IPointer::SAxisEvent& event);
        void                               onFrame();

        CBox                               monitorBox() const;
        void                               clampPointer();
        void                               updatePointerFocus(uint32_t timeMs);
        void                               setPointerFocus(SP<CWLSurfaceResource> surface, const Vector2D& local);
        void                               setKeyboardSurface(SP<CWLSurfaceResource> surface);
        void                               sendSelection(const wl_client* client, bool primary);
        void                               endPopupGrab();
        bool                               popupGrabActive();

        ActorID                            m_actor = HUMAN_ACTOR_ID;

        std::vector<SKeyboard>             m_keyboards;
        std::vector<SPointer>              m_pointers;
        WP<IKeyboard>                      m_activeKeyboard;

        Vector2D                           m_pointerPosition;
        bool                               m_pointerPlaced = false;
        std::vector<uint32_t>              m_pressedButtons;

        WP<CWLSurfaceResource>             m_keyboardFocus;
        WP<CWLSurfaceResource>             m_pointerFocus;
        Vector2D                           m_pointerFocusOrigin;
        PHLWINDOWREF                       m_pointerWindow;
        PHLWINDOWREF                       m_focusedWindow;

        CHyprSignalListener                m_keyboardFocusDestroy;
        CHyprSignalListener                m_pointerFocusDestroy;

        SSelection                         m_selection;
        SSelection                         m_primarySelection;

        SP<CSeatGrab>                      m_popupGrab;
        std::vector<WP<CXDGPopupResource>> m_grabbedPopups;
    };
}
