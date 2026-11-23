#include "S2.hpp"

#include "Unix.hpp"
#include "../../actors/Actors.hpp"

#include <algorithm>
#include <format>

using namespace IPC;
using namespace IPC::Socket2;

UP<CSocket2>& IPC::Socket2::sock() {
    static auto p = makeUnique<CSocket2>();
    return p;
}

CSocket2::CSocket2(std::string directory) : m_impl(makeUnique<CUnixImpl>(std::move(directory))) {
    ;
}

static std::string formatEvent(const SEvent& event) {
    std::string_view data        = event.data;
    const auto       LEN         = event.event.length();
    auto             eventString = std::format("{}>>{}\n", event.event, data.substr(0, 1024));
    std::replace(eventString.begin() + LEN + 2, eventString.end() - 1, '\n', ' ');
    return eventString;
}

void CSocket2::postEvent(SEvent&& event) {
    if (const auto AUDIENCE = Actors::eventAudience(event.subject); AUDIENCE != Actors::HUMAN_ACTOR_ID) {
        Actors::postAgentEvent(AUDIENCE, event);
        return;
    }

    send(event);
}

void CSocket2::send(const SEvent& event) {
    m_impl->send(formatEvent(event));
}
