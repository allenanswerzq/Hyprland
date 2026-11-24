#include "Actors.hpp"

#include "ActorRegistry.hpp"
#include "AgentManager.hpp"

#include "../desktop/state/ViewState.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../ipc/s1/S1.hpp"

#include <hyprutils/string/VarList.hpp>

#include <format>

using namespace Actors;
using namespace IPC::Socket1;
using namespace Hyprutils::String;

// hyprctl actors: every actor with its Wayland socket and how many clients it has.
static SResponse actorsRequest(const SRequest& request) {
    const bool JSON = request.format == FORMAT_JSON;
    if (!g_pActorRegistry)
        return JSON ? "[]" : "actors are unavailable";

    std::string result;
    if (JSON) {
        result += "[";
        for (const auto& actor : g_pActorRegistry->actors()) {
            result += std::format(
                R"#(
{{
    "id": {},
    "kind": "{}",
    "name": "{}",
    "waylandSocket": "{}",
    "clients": {}
}},)#",
                actor.id, actorKindToString(actor.kind), escapeJSONStrings(actor.name), escapeJSONStrings(actor.waylandSocket), g_pActorRegistry->clientCount(actor.id));
        }

        if (result.ends_with(','))
            result.pop_back();
        result += "]";
        return result;
    }

    for (const auto& actor : g_pActorRegistry->actors()) {
        result += std::format("Actor {} ({}):\n\tkind: {}\n\twaylandSocket: {}\n\tclients: {}\n\n", actor.name, actor.id, actorKindToString(actor.kind), actor.waylandSocket,
                              g_pActorRegistry->clientCount(actor.id));
    }

    return result;
}

// hyprctl actor borrow <window> and hyprctl actor return <window>. The window is any window
// selector, for example address:0x1234.
static SResponse actorRequest(const SRequest& request) {
    CVarList   vars(request.command, 0, ' ');

    const auto RESULT = [&request](const std::expected<void, std::string>& result) -> SResponse {
        if (request.format == FORMAT_JSON)
            return result ? R"({"ok": true})" : std::format(R"({{"ok": false, "error": "{}"}})", escapeJSONStrings(result.error()));
        return result ? "ok" : result.error();
    };

    if (vars.size() != 3 || (vars[1] != "borrow" && vars[1] != "return"))
        return RESULT(std::unexpected("usage: actor borrow|return <window>"));

    if (!g_pAgentManager)
        return RESULT(std::unexpected("there are no agents"));

    const auto WINDOW = Desktop::viewState()->query().selector(vars[2]).runWindow();
    if (!WINDOW)
        return RESULT(std::unexpected("window not found"));

    return RESULT(vars[1] == "borrow" ? g_pAgentManager->borrow(WINDOW) : g_pAgentManager->returnWindow(WINDOW));
}

void Actors::registerCommands(CSocket1& socket) {
    socket.registerCommand({.name = "actors", .match = COMMAND_MATCH_EXACT, .handler = actorsRequest});
    socket.registerCommand({.name = "actor ", .match = COMMAND_MATCH_PREFIX, .handler = actorRequest});
}
