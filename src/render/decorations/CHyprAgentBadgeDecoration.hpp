#pragma once

#include <string>

#include "IHyprWindowDecoration.hpp"

namespace Render {
    class ITexture;
}

/*
    Marks a window the human borrowed from an agent: a badge with the agent's name, such as
    agent-001, in the window's top right corner. It is drawn above the window, under its popups
    (the overlay layer is only drawn for the focused tiled window), and in fullscreen too, so a
    borrowed window never passes for one of the human's own.

    Every window has one. It draws only while the window is borrowed, so it follows borrows,
    returns, and windows a borrowed app opens later, without being added or removed.
*/
class CHyprAgentBadgeDecoration : public IHyprWindowDecoration {
  public:
    CHyprAgentBadgeDecoration(PHLWINDOW);
    virtual ~CHyprAgentBadgeDecoration() = default;

    virtual SDecorationPositioningInfo getPositioningInfo() override;

    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply) override;

    virtual void                       draw(Render::CRenderContext& ctx, PHLMONITOR, float const& a, const Render::SWindowRenderPresentation& presentation) override;

    virtual eDecorationType            getDecorationType() override;

    virtual void                       updateWindow(PHLWINDOW) override;

    virtual void                       damageEntire() override;

    virtual eDecorationLayer           getDecorationLayer() override;

    virtual uint64_t                   getDecorationFlags() override;

    virtual std::string                getDisplayName() override;

  private:
    // The badge text, rendered once per text and monitor scale.
    SP<Render::ITexture> textTexture(const std::string& text, float scale);

    PHLWINDOWREF         m_window;
    SP<Render::ITexture> m_textTexture;
    std::string          m_text;
    float                m_textScale = 0.F;
};
