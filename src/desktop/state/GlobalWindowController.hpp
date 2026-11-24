#pragma once

#include "../DesktopTypes.hpp"

namespace Desktop {
    class CGlobalWindowController {
      public:
        void updateAllWindowsDecorations() const;
        void updateSuspendedStates() const;
        // Suspends window if nobody can see it, or resumes it if someone can.
        void updateSuspendedState(const PHLWINDOW& window) const;
        void moveWindowToWorkspace(PHLWINDOW pWindow, PHLWORKSPACE pWorkspace) const;
    };

    UP<CGlobalWindowController>& globalWindowController();
};
