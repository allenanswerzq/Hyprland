#include "PointerController.hpp"

#include "../state/MonitorState.hpp"
#include "../config/ConfigValue.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../managers/ActorManager.hpp"

#include "PointerManager.hpp"

using namespace Pointer;

UP<CPointerController>& Pointer::pointerController() {
    static UP<CPointerController> p = makeUnique<CPointerController>();
    return p;
}

void CPointerController::warpTo(const Vector2D& pos, bool force) const {
    static auto PNOWARPS = CConfigValue<Config::INTEGER>("cursor:no_warps");

    if (*PNOWARPS && !force) {
        const auto PMONITORNEW = State::monitorState()->query().vec(pos).run();
        Desktop::focusState()->rawMonitorFocus(PMONITORNEW);
        return;
    }

    // an agent's warps move its own cursor, not the human's
    if (const auto ACTOR = Actor::mgr()->current(); !ACTOR->isHuman()) {
        ACTOR->warpPointer(pos);
        return;
    }

    Pointer::mgr()->warpTo(pos);

    const auto PMONITORNEW = State::monitorState()->query().vec(pos).run();
    Desktop::focusState()->rawMonitorFocus(PMONITORNEW);
}
