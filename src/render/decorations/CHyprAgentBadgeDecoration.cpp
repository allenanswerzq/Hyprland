#include "CHyprAgentBadgeDecoration.hpp"

#include <cmath>

#include "../../Compositor.hpp"
#include "../../actors/Actors.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/WindowPresentation.hpp"
#include "../../output/Monitor.hpp"
#include "../../workspace/HLWorkspace.hpp"
#include "../pass/RectPassElement.hpp"
#include "../pass/TexPassElement.hpp"
#include "../Renderer.hpp"
#include "../Texture.hpp"
#include "../WindowRenderPresentation.hpp"

// In logical pixels, scaled by the monitor's scale.
constexpr double        BADGE_MARGIN      = 8.0;
constexpr double        BADGE_PADDING_X   = 8.0;
constexpr double        BADGE_PADDING_Y   = 3.0;
constexpr int           BADGE_FONT_SIZE   = 10;
constexpr int           BADGE_FONT_WEIGHT = 700;

static const CHyprColor BADGE_COLOR      = CHyprColor(0.96F, 0.62F, 0.12F, 0.92F);
static const CHyprColor BADGE_TEXT_COLOR = CHyprColor(0.08F, 0.08F, 0.08F, 1.F);

CHyprAgentBadgeDecoration::CHyprAgentBadgeDecoration(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_window(pWindow) {
    ;
}

eDecorationType CHyprAgentBadgeDecoration::getDecorationType() {
    return DECORATION_AGENT_BADGE;
}

// The badge sits inside the window, so it takes no space and moves nothing.
SDecorationPositioningInfo CHyprAgentBadgeDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.policy = DECORATION_POSITION_ABSOLUTE;
    return info;
}

void CHyprAgentBadgeDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    ;
}

void CHyprAgentBadgeDecoration::updateWindow(PHLWINDOW pWindow) {
    ;
}

uint64_t CHyprAgentBadgeDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

eDecorationLayer CHyprAgentBadgeDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

std::string CHyprAgentBadgeDecoration::getDisplayName() {
    return "Agent Badge";
}

void CHyprAgentBadgeDecoration::damageEntire() {
    const auto PWINDOW = m_window.lock();
    if (!validMapped(PWINDOW) || !Actors::lenderOf(PWINDOW))
        return;

    CBox       windowBox = PWINDOW->getWindowMainSurfaceBox();

    const auto PWORKSPACE = PWINDOW->presentation().workspace();
    if (PWORKSPACE && PWORKSPACE->m_renderOffset->isBeingAnimated() && !(PWINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED))
        windowBox.translate(PWORKSPACE->m_renderOffset->value());
    windowBox.translate(PWINDOW->presentation().floatingOffset());

    g_pHyprRenderer->damageRegion(CRegion(windowBox));
}

SP<Render::ITexture> CHyprAgentBadgeDecoration::textTexture(const std::string& text, float scale) {
    if (m_textTexture && m_text == text && m_textScale == scale)
        return m_textTexture;

    static auto FONT_FAMILY = CConfigValue<Config::STRING>("misc:font_family");

    m_textTexture = g_pHyprRenderer->renderText(text, BADGE_TEXT_COLOR, sc<int>(std::round(BADGE_FONT_SIZE * scale)), false, *FONT_FAMILY, 0, BADGE_FONT_WEIGHT);
    m_text        = text;
    m_textScale   = scale;
    return m_textTexture;
}

void CHyprAgentBadgeDecoration::draw(Render::CRenderContext& ctx, PHLMONITOR pMonitor, float const& a, const Render::SWindowRenderPresentation& presentation) {
    const auto PWINDOW = m_window.lock();
    if (!validMapped(PWINDOW))
        return;

    const auto LENDER = Actors::lenderOf(PWINDOW);
    if (!LENDER)
        return;

    const float SCALE   = pMonitor->m_scale;
    const auto  TEXTURE = textTexture(Actors::nameOf(*LENDER), SCALE);
    if (!TEXTURE || TEXTURE->m_size.x < 1 || TEXTURE->m_size.y < 1)
        return;

    CBox windowBox = PWINDOW->getWindowMainSurfaceBox();
    windowBox.translate(-pMonitor->m_position + presentation.workspaceOffset + presentation.floatingOffset).scale(SCALE).round();

    const Vector2D PADDING = Vector2D{BADGE_PADDING_X, BADGE_PADDING_Y} * SCALE;
    const double   MARGIN  = BADGE_MARGIN * SCALE;
    const Vector2D SIZE    = TEXTURE->m_size + PADDING * 2;

    // Too small a window shows no badge rather than one that covers it.
    if (SIZE.x + (MARGIN * 2) > windowBox.w || SIZE.y + (MARGIN * 2) > windowBox.h)
        return;

    CBox badgeBox = {windowBox.x + windowBox.w - MARGIN - SIZE.x, windowBox.y + MARGIN, SIZE.x, SIZE.y};
    badgeBox.round();

    CRectPassElement::SRectData rect;
    rect.box   = badgeBox;
    rect.color = BADGE_COLOR;
    rect.color.a *= a;
    rect.round = sc<int>(std::round(SIZE.y / 2));
    g_pHyprRenderer->addPassElement(ctx, makeUnique<CRectPassElement>(rect));

    CTexPassElement::SRenderData text;
    text.tex = TEXTURE;
    text.box = CBox{badgeBox.pos() + PADDING, TEXTURE->m_size}.round();
    text.a   = a;
    g_pHyprRenderer->addPassElement(ctx, makeUnique<CTexPassElement>(std::move(text)));
}
