#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>

#include "../desktop/DesktopTypes.hpp"

namespace Actors {
    using ActorID = uint32_t;

    inline constexpr ActorID HUMAN_ACTOR_ID  = 0;
    inline constexpr size_t  MAX_AGENT_COUNT = 64;

    // Agent monitors live far away from the human layout, so geometric lookups such as
    // "which monitor contains this point" never mix human and agent space.
    inline constexpr double AGENT_LAYOUT_ORIGIN_X = 1048576.0;
    inline constexpr double AGENT_LAYOUT_ORIGIN_Y = 1048576.0;
    inline constexpr double AGENT_LAYOUT_STRIDE   = 65536.0;

    // What a socket2 event is about. The event goes to the socket2 of the actor whose desktop
    // the subject is on; an ActorID names the actor directly. No subject means the human.
    using SEventSubject = std::variant<std::monostate, ActorID, PHLWINDOW, PHLWORKSPACE, PHLMONITOR, PHLLS>;
}
