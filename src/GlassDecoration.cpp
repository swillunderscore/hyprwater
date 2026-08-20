#include "GlassDecoration.hpp"
#include "BuiltInPresets.hpp"
#include "GlassPassElement.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

CGlassDecoration::CGlassDecoration(PHLWINDOW window)
    : IHyprWindowDecoration(window), m_window(window) {
}

bool CGlassDecoration::resolveEnabled() const {
    const auto& config = g_pGlobalState->config;
    const bool globalEnabled = config.enabled && **config.enabled;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const auto& tags = window->m_ruleApplicator->m_tagKeeper;
            // Disabled tag wins over enabled tag if both are present.
            if (tags.isTagged(std::string(TAG_DISABLED)))
                return false;
            if (tags.isTagged(std::string(TAG_ENABLED)))
                return true;
        }
    } catch (...) {}

    return globalEnabled;
}

bool CGlassDecoration::resolveThemeIsDark() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const std::string lightTag = std::string(TAG_THEME_PREFIX) + "light";
            const std::string darkTag  = std::string(TAG_THEME_PREFIX) + "dark";
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(lightTag))
                return false;
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(darkTag))
                return true;
        }

        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassDecoration::resolvePresetName() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
                if (tag.starts_with(TAG_PRESET_PREFIX))
                    return tag.substr(TAG_PRESET_PREFIX.size());
            }
        }

        const auto& config = g_pGlobalState->config;
        const auto preset = readStringConfig(config.defaultPreset);
        if (!preset.empty())
            return std::string(preset);
    } catch (...) {}

    return "default";
}

SDecorationPositioningInfo CGlassDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.priority       = 10000;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {}

void CGlassDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    if (const auto w = m_window.lock())
        GlassRenderer::DBG_LOG("DRAWCALL win=%lx x=%.0f y=%.0f\n",
                               reinterpret_cast<uintptr_t>(w.get()),
                               w->positionAnimation()->value().x,
                               w->positionAnimation()->value().y);
    if (!g_pGlobalState || !resolveEnabled())
        return;

    // With the adaptive dim on, the glass must redraw WHOLE every frame. Damage
    // only re-renders part of it, so the rest keeps pixels rendered at an
    // earlier moment — and the compositor alternates swapchain buffers holding
    // those two moments. Undimmed the two are near enough to be invisible;
    // dimming widens the gap between them and the boundary row visibly toggles
    // between two values. Costs a full redraw per frame (~3% GPU here) and buys
    // a stable edge.
    if (const auto& adCfg = g_pGlobalState->config;
        adCfg.adaptiveTint && **adCfg.adaptiveTint > 0.001f) {
        const bool termOnly =
            adCfg.adaptiveTintTerminalsOnly && **adCfg.adaptiveTintTerminalsOnly;
        const auto w = m_window.lock();
        if (!termOnly || !w || isTerminalClass(w->m_class))
            damageEntire();
    }

    CGlassPassElement::SGlassPassData data{m_self, alpha};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPassElement>(data));

    const auto window = m_window.lock();
    if (window) {
        const auto workspace = window->m_workspace;

        const bool wsAnimating = workspace && !window->m_pinned && workspace->m_renderOffset->isBeingAnimated();
        if (wsAnimating)
            damageEntire();

        const auto currentPosition = window->positionAnimation()->value();
        const auto currentSize = window->sizeAnimation()->value();
        const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;
        if (moved) {
            damageEntire();
            m_lastPosition = currentPosition;
            m_lastSize = currentSize;
        }

        // Bump layer cache only for actual scene changes (window moved/animating),
        // NOT from damageEntire() which fires in the damage system feedback path.
        if (moved || wsAnimating) {
            if (auto mon = window->m_monitor.lock())
                g_pGlobalState->bumpSceneGeneration(mon.get());
        }
    }
}

PHLWINDOW CGlassDecoration::getOwner() {
    return m_window.lock();
}

void CGlassDecoration::renderPass(PHLMONITOR monitor, const float& alpha) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto window = m_window.lock();
    if (!window) {
        GlassRenderer::DBG_LOG("RP bail=nowindow\n");
        return;
    }
    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source) {
        GlassRenderer::DBG_LOG("RP win=%lx bail=nosource\n", reinterpret_cast<uintptr_t>(window.get()));
        return;
    }

    auto optBox = WindowGeometry::computeWindowBox(window, monitor);
    if (!optBox) {
        GlassRenderer::DBG_LOG("RP win=%lx bail=nobox\n", reinterpret_cast<uintptr_t>(window.get()));
        return;
    }

    CBox windowBox    = *optBox;
    CBox transformBox = windowBox;

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    transformBox.transform(transform,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);

    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
    int downscale        = GlassRenderer::blurDownscale(blurStrength);

    GlassRenderer::sampleBackground(m_sampleFramebuffer, source, transformBox, m_samplePaddingRatio, downscale);

    float blurRadius     = blurStrength * 12.0f / downscale;
    int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);
    int viewportWidth    = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x);
    int viewportHeight   = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);
    GlassRenderer::blurBackground(m_sampleFramebuffer, m_blurTempFramebuffer, blurRadius, blurIterations, dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(), viewportWidth, viewportHeight);

    // One time-smoothed average for the whole window, so the dim is uniform and
    // does not pump with an animated wallpaper.
    const auto& lumaCfg = g_pGlobalState->config;
    const bool adaptiveAllowed =
        !(lumaCfg.adaptiveTintTerminalsOnly && **lumaCfg.adaptiveTintTerminalsOnly) ||
        isTerminalClass(window->m_class);
    SP<Render::IFramebuffer> adaptiveLumaFb = m_lumaFb[m_lumaCurrent];
    if (adaptiveAllowed) {
        GlassRenderer::updateAdaptiveLuma(m_sampleFramebuffer, m_lumaFb, m_lumaCurrent, m_lumaSeeded,
                                          dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(),
                                          viewportWidth, viewportHeight);
    } else {
        adaptiveLumaFb = nullptr;
    }

    GlassRenderer::DBG_LOG("RP win=%lx alpha=%.3f BLURRED str=%.2f rad=%.2f it=%d sample=%.0fx%.0f srcfb=%u box=%.0f,%.0f %.0fx%.0f\n",
                           reinterpret_cast<uintptr_t>(window.get()), alpha, blurStrength, blurRadius, blurIterations,
                           m_sampleFramebuffer ? m_sampleFramebuffer->m_size.x : -1.0,
                           m_sampleFramebuffer ? m_sampleFramebuffer->m_size.y : -1.0,
                           dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(),
                           windowBox.x, windowBox.y, windowBox.width, windowBox.height);

    float monitorScale  = monitor->m_scale;
    float cornerRadius  = window->rounding() * monitorScale;
    float roundingPower = window->roundingPower();

    GlassRenderer::applyGlassEffect(m_sampleFramebuffer, source,
                                     windowBox, transformBox, alpha,
                                     cornerRadius, roundingPower, m_samplePaddingRatio, ctx,
                                     nullptr, adaptiveLumaFb);
}

eDecorationType CGlassDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGlassDecoration::updateWindow(PHLWINDOW window) {
    damageEntire();
}

void CGlassDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!window)
        return;

    const auto workspace = window->m_workspace;
    auto surfaceBox = window->getWindowMainSurfaceBox();

    if (workspace && workspace->m_renderOffset->isBeingAnimated() && !window->m_pinned)
        surfaceBox.translate(workspace->m_renderOffset->value());
    surfaceBox.translate(window->m_floatingOffset);

    // Expand damage by our sampling padding so the render pass re-renders
    // background content (wallpaper, other windows) in the padded margin.
    // Without this, the scissored render pass leaves stale previous-frame
    // content in the padding area, causing noise artifacts.
    // surfaceBox is in logical coords; convert pixel padding to logical.
    const auto monitor = window->m_monitor.lock();
    const float scale = monitor ? monitor->m_scale : 1.0f;
    surfaceBox.expand(GlassRenderer::SAMPLE_PADDING_PX / scale);

    g_pHyprRenderer->damageBox(surfaceBox);
}

eDecorationLayer CGlassDecoration::getDecorationLayer() {
    return DECORATION_LAYER_BOTTOM;
}

uint64_t CGlassDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlassDecoration::getDisplayName() {
    return "HyprGlass";
}
