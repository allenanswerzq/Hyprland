#pragma once

#include <vector>

#include "../desktop/DesktopTypes.hpp"
#include "MonitorQuery.hpp"

namespace State {
    class IMonitorStateTracker {
      public:
        virtual ~IMonitorStateTracker() = default;

        // The human's monitors: allMonitors() has every one, monitors() the enabled ones
        // that are not mirrors. Agent monitors (src/actors) are not part of the human's
        // layout, focus, pointer or fallback, so they are kept apart in agentMonitors().
        // contains() knows every monitor, human or agent.
        virtual const std::vector<PHLMONITOR>& allMonitors() const = 0;
        virtual const std::vector<PHLMONITOR>& monitors() const    = 0;
        // Every agent monitor, enabled or not.
        virtual const std::vector<PHLMONITOR>& agentMonitors() const = 0;
        // Every monitor that draws frames: monitors() and the enabled agent monitors. For
        // damage and frame pacing, which follow a surface wherever it is shown.
        virtual const std::vector<PHLMONITOR>& renderMonitors() const = 0;

        virtual CMonitorQuery                  query() const;
        virtual bool                           contains(PHLMONITOR monitor) const;

      protected:
        IMonitorStateTracker() = default;
    };
}
