#include "WindowEmbeds.hpp"

#include <algorithm>

#include "../desktop/view/window/Window.hpp"
#include "../desktop/state/GlobalWindowController.hpp"
#include "../event/EventBus.hpp"
#include "../render/Renderer.hpp"

using namespace Actors;

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
    if (IT == m_embeds.end())
        return std::unexpected("the window is not embedded");

    damageBox(*IT);
    m_embeds.erase(IT);
    window->updateSurfaceScaleTransformDetails(true);
    Desktop::globalWindowController()->updateSuspendedState(window);
    return {};
}

std::vector<CWindowEmbeds::SEmbed> CWindowEmbeds::embeddedIn(const PHLWINDOW& host) const {
    std::vector<SEmbed> result;
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

void CWindowEmbeds::damage(const PHLWINDOW& window) const {
    if (const auto EMBED = embedOf(window))
        damageBox(*EMBED);
}

const CWindowEmbeds::SEmbed* CWindowEmbeds::embedOf(const PHLWINDOW& window) const {
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
