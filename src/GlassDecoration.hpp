#pragma once

#include "GlassRenderer.hpp"
#include "PluginConfig.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

class CGlassDecoration : public IHyprWindowDecoration {
  public:
    explicit CGlassDecoration(PHLWINDOW window);
    ~CGlassDecoration() override = default;

    [[nodiscard]] SDecorationPositioningInfo getPositioningInfo() override;
    void                                     onPositioningReply(const SDecorationPositioningReply& reply) override;
    void                                     draw(PHLMONITOR monitor, float const& alpha) override;
    [[nodiscard]] eDecorationType            getDecorationType() override;
    void                                     updateWindow(PHLWINDOW window) override;
    void                                     damageEntire() override;
    [[nodiscard]] eDecorationLayer           getDecorationLayer() override;
    [[nodiscard]] uint64_t                   getDecorationFlags() override;
    [[nodiscard]] std::string                getDisplayName() override;

    [[nodiscard]] PHLWINDOW getOwner();
    void                    renderPass(PHLMONITOR monitor, const float& alpha);

    WP<CGlassDecoration> m_self;

  private:
    PHLWINDOWREF m_window;
    SP<Render::IFramebuffer> m_sampleFramebuffer;
    // Per-window blur ping-pong scratch. Sized to THIS window's sample, so it
    // must not be shared with other decorations — see blurBackground().
    SP<Render::IFramebuffer> m_blurTempFramebuffer;
    // 1x1 ping-pong holding this window's time-smoothed average backdrop luma.
    // A fragment shader has no memory between frames; this is that memory.
    SP<Render::IFramebuffer> m_lumaFb[2];
    int  m_lumaCurrent = 0;
    bool m_lumaSeeded  = false;
    Vector2D     m_samplePaddingRatio;

    // Track last rendered position/size to detect actual changes and seed damage
    Vector2D m_lastPosition;
    Vector2D m_lastSize;

    [[nodiscard]] bool        resolveEnabled() const;
    [[nodiscard]] bool        resolveThemeIsDark() const;
    [[nodiscard]] std::string resolvePresetName() const;

    friend class CGlassPassElement;
};
