#include "WindowEmbeds.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "../desktop/view/window/Window.hpp"
#include "../desktop/state/GlobalWindowController.hpp"
#include "../desktop/state/ViewState.hpp"
#include "../event/EventBus.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../ipc/s1/S1.hpp"
#include "../render/Renderer.hpp"

#include <hyprutils/string/VarList.hpp>

using namespace Hyprutils::String;

namespace Embed {
    class CWindowEmbeds {
      public:
        CWindowEmbeds();

        CWindowEmbeds(const CWindowEmbeds&)                              = delete;
        CWindowEmbeds&                   operator=(const CWindowEmbeds&) = delete;

        std::expected<void, std::string> embed(PHLWINDOW window, PHLWINDOW host, const CBox& box, const Vector2D& source);
        std::expected<void, std::string> unembed(PHLWINDOW window);
        std::vector<SEmbed>              embeddedIn(const PHLWINDOW& host) const;
        PHLWINDOW                        hostOf(const PHLWINDOW& window) const;
        bool                             damage(const PHLWINDOW& window) const;

      private:
        const SEmbed*       embedOf(const PHLWINDOW& window) const;
        void                damageBox(const SEmbed& embed) const;
        void                onWindowClosed(const PHLWINDOW& window);

        std::vector<SEmbed> m_embeds;
        CHyprSignalListener m_windowClose;
    };
}

using namespace Embed;

static UP<CWindowEmbeds> g_pWindowEmbeds;

CWindowEmbeds::CWindowEmbeds() {
    m_windowClose = Event::bus()->m_events.window.close.listen([this](const PHLWINDOW& window) { onWindowClosed(window); });
}

std::expected<void, std::string> CWindowEmbeds::embed(PHLWINDOW window, PHLWINDOW host, const CBox& box, const Vector2D& source) {
    if (!window || !host)
        return std::unexpected("window not found");

    if (window == host)
        return std::unexpected("a window cannot be embedded in itself");

    if (!window->mapped() || !host->mapped())
        return std::unexpected("both windows must be mapped");

    if (box.w < 1 || box.h < 1)
        return std::unexpected("the box is empty");

    if (hostOf(host))
        return std::unexpected("the host is itself embedded");

    if (!embeddedIn(window).empty())
        return std::unexpected("the window is a host");

    if (const auto OLD = std::ranges::find_if(m_embeds, [&window](const auto& e) { return e.window.lock() == window; }); OLD != m_embeds.end()) {
        damageBox(*OLD);
        m_embeds.erase(OLD);
    }

    m_embeds.emplace_back(SEmbed{.window = window, .host = host, .box = box, .source = source});
    damageBox(m_embeds.back());

    // The embedded window renders for the host's monitor from now on, and is seen when the host is.
    window->updateSurfaceScaleTransformDetails(true);
    Desktop::globalWindowController()->updateSuspendedState(window);
    return {};
}

std::expected<void, std::string> CWindowEmbeds::unembed(PHLWINDOW window) {
    const auto IT = std::ranges::find_if(m_embeds, [&window](const auto& e) { return e.window.lock() == window; });
    if (!window || IT == m_embeds.end())
        return std::unexpected("the window is not embedded");

    damageBox(*IT);
    m_embeds.erase(IT);
    window->updateSurfaceScaleTransformDetails(true);
    Desktop::globalWindowController()->updateSuspendedState(window);
    return {};
}

std::vector<SEmbed> CWindowEmbeds::embeddedIn(const PHLWINDOW& host) const {
    std::vector<SEmbed> result;
    if (!host)
        return result;

    for (const auto& embed : m_embeds) {
        if (embed.host.lock() == host && embed.window)
            result.emplace_back(embed);
    }

    return result;
}

PHLWINDOW CWindowEmbeds::hostOf(const PHLWINDOW& window) const {
    const auto EMBED = embedOf(window);
    return EMBED ? EMBED->host.lock() : nullptr;
}

bool CWindowEmbeds::damage(const PHLWINDOW& window) const {
    const auto EMBED = embedOf(window);
    if (!EMBED)
        return false;

    damageBox(*EMBED);
    return true;
}

const SEmbed* CWindowEmbeds::embedOf(const PHLWINDOW& window) const {
    if (!window)
        return nullptr;

    const auto IT = std::ranges::find_if(m_embeds, [&window](const auto& e) { return e.window.lock() == window; });
    return IT == m_embeds.end() || !IT->host ? nullptr : &*IT;
}

void CWindowEmbeds::damageBox(const SEmbed& embed) const {
    const auto HOST = embed.host.lock();
    if (!HOST || !g_pHyprRenderer)
        return;

    g_pHyprRenderer->damageBox(embed.box.copy().translate(HOST->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT)));
}

void CWindowEmbeds::onWindowClosed(const PHLWINDOW& window) {
    std::vector<PHLWINDOW> released;
    std::erase_if(m_embeds, [&window, &released](const auto& embed) {
        if (embed.window && embed.window.lock() != window && embed.host.lock() != window)
            return false;

        if (const auto EMBEDDED = embed.window.lock(); EMBEDDED && EMBEDDED != window)
            released.emplace_back(EMBEDDED);
        return true;
    });

    // Windows whose host closed render for their own monitor again, and are seen only there.
    for (const auto& w : released) {
        w->updateSurfaceScaleTransformDetails(true);
        Desktop::globalWindowController()->updateSuspendedState(w);
    }
}

void Embed::init() {
    g_pWindowEmbeds = makeUnique<CWindowEmbeds>();
}

void Embed::destroy() {
    g_pWindowEmbeds.reset();
}

std::expected<void, std::string> Embed::embed(PHLWINDOW window, PHLWINDOW host, const CBox& box, const Vector2D& source) {
    if (!g_pWindowEmbeds)
        return std::unexpected("embedding is unavailable");

    return g_pWindowEmbeds->embed(std::move(window), std::move(host), box, source);
}

std::expected<void, std::string> Embed::unembed(PHLWINDOW window) {
    if (!g_pWindowEmbeds)
        return std::unexpected("embedding is unavailable");

    return g_pWindowEmbeds->unembed(std::move(window));
}

std::vector<SEmbed> Embed::embeddedIn(const PHLWINDOW& host) {
    return g_pWindowEmbeds ? g_pWindowEmbeds->embeddedIn(host) : std::vector<SEmbed>{};
}

bool Embed::isHost(const PHLWINDOW& window) {
    return !embeddedIn(window).empty();
}

PHLWINDOW Embed::hostOf(const PHLWINDOW& window) {
    return g_pWindowEmbeds ? g_pWindowEmbeds->hostOf(window) : nullptr;
}

bool Embed::damageThroughHost(const PHLWINDOW& window) {
    return g_pWindowEmbeds && g_pWindowEmbeds->damage(window);
}

static std::string commandResult(IPC::Socket1::eOutputFormat format, const std::expected<void, std::string>& result) {
    if (format == IPC::Socket1::FORMAT_JSON)
        return result ? R"({"ok": true})" : std::format(R"({{"ok": false, "error": "{}"}})", escapeJSONStrings(result.error()));

    return result ? "ok" : result.error();
}

static PHLWINDOW windowFromSelector(const std::string& selector) {
    return Desktop::viewState()->query().selector(selector).runWindow();
}

// embed <window> <host> <x> <y> <width> <height> <sourceX> <sourceY>: shows window inside host,
// in the box x, y, width, height of the host, with the point sourceX, sourceY of window at the
// box's top left corner. Windows are any window selector, for example address:0x1234. All
// numbers are logical pixels.
static IPC::Socket1::SResponse embedRequest(const IPC::Socket1::SRequest& request) {
    CVarList vars(request.command, 0, ' ');
    if (vars.size() != 9)
        return commandResult(request.format, std::unexpected("usage: embed <window> <host> <x> <y> <width> <height> <sourceX> <sourceY>"));

    std::array<double, 6> numbers = {};
    for (size_t i = 0; i < numbers.size(); ++i) {
        try {
            numbers[i] = std::stod(vars[i + 3]);
        } catch (...) { return commandResult(request.format, std::unexpected(std::format("not a number: {}", vars[i + 3]))); }
    }

    return commandResult(
        request.format,
        Embed::embed(windowFromSelector(vars[1]), windowFromSelector(vars[2]), CBox{numbers[0], numbers[1], numbers[2], numbers[3]}, Vector2D{numbers[4], numbers[5]}));
}

// unembed <window>
static IPC::Socket1::SResponse unembedRequest(const IPC::Socket1::SRequest& request) {
    CVarList vars(request.command, 0, ' ');
    if (vars.size() != 2)
        return commandResult(request.format, std::unexpected("usage: unembed <window>"));

    const auto WINDOW = windowFromSelector(vars[1]);
    if (!WINDOW)
        return commandResult(request.format, std::unexpected("window not found"));

    return commandResult(request.format, Embed::unembed(WINDOW));
}

void Embed::registerCommands(IPC::Socket1::CSocket1& socket) {
    socket.registerCommand({.name = "embed", .match = IPC::Socket1::COMMAND_MATCH_PREFIX, .handler = embedRequest});
    socket.registerCommand({.name = "unembed", .match = IPC::Socket1::COMMAND_MATCH_PREFIX, .handler = unembedRequest});
}
