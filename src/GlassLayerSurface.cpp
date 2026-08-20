#include "GlassLayerSurface.hpp"
#include "BuiltInPresets.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "LayerGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <hyprland/src/desktop/Workspace.hpp>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

static CBox transformedLayerBox(CBox pixelBox, PHLMONITOR monitor) {
    const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
    pixelBox.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y).noNegativeSize().round();
    return pixelBox;
}

CGlassLayerSurface::CGlassLayerSurface(PHLLS layerSurface)
    : m_layerSurface(layerSurface) {
}

CGlassLayerSurface::~CGlassLayerSurface() {
    // Damage the area where glass was last drawn so the compositor
    // re-renders it without the glass effect (prevents ghost artifacts).
    if (g_pHyprRenderer && m_lastSize.x > 0 && m_lastSize.y > 0 &&
        std::isfinite(m_lastPosition.x) && std::isfinite(m_lastPosition.y) &&
        std::isfinite(m_lastSize.x) && std::isfinite(m_lastSize.y)) {
        auto box = CBox{m_lastPosition, m_lastSize};
        box.expand(GlassRenderer::SAMPLE_PADDING_PX).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);
    }
}

bool CGlassLayerSurface::resolveThemeIsDark() const {
    try {
        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassLayerSurface::resolvePresetName() const {
    try {
        // Per-namespace preset override (highest priority)
        const auto layerSurface = m_layerSurface.lock();
        if (layerSurface) {
            const auto& nsPresets = g_pGlobalState->layerNamespacePresets;
            auto it = nsPresets.find(layerSurface->m_namespace);
            if (it != nsPresets.end())
                return it->second;
        }

        const auto& config = g_pGlobalState->config;

        // Layer-wide preset override
        const auto layerPreset = readStringConfig(config.layersPreset);
        if (!layerPreset.empty())
            return std::string(layerPreset);

        // Fall back to global default preset
        const auto defaultPreset = readStringConfig(config.defaultPreset);
        if (!defaultPreset.empty())
            return std::string(defaultPreset);
    } catch (...) {}

    return "default";
}

PHLLS CGlassLayerSurface::getLayerSurface() const {
    return m_layerSurface.lock();
}

void CGlassLayerSurface::damageIfMoved() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto currentPosition = layerSurface->positionAnimation()->value();
    const auto currentSize     = layerSurface->sizeAnimation()->value();
    if (currentSize.x <= 0.0 || currentSize.y <= 0.0 ||
        !std::isfinite(currentPosition.x) || !std::isfinite(currentPosition.y) ||
        !std::isfinite(currentSize.x) || !std::isfinite(currentSize.y))
        return;

    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha().isBeingAnimated() ||
                             !layerSurface->m_mapped;   // 0.56: fade-outs are separate snapshots now

    const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;

    if (moved || isAnimating) {
        m_lastPosition  = currentPosition;
        m_lastSize      = currentSize;

        auto box = CBox{currentPosition, currentSize};
        const auto monitor = layerSurface->m_monitor.lock();
        const float scale = monitor ? monitor->m_scale : 1.0f;
        box.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);

        if (monitor)
            g_pGlobalState->bumpSceneGeneration(monitor.get());
    }
}

void CGlassLayerSurface::sampleAndRedirect(PHLMONITOR monitor, float alpha) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return;

    CBox transformBox = transformedLayerBox(*layerBox, monitor);

    // Decide whether we need to re-sample and re-blur the background.
    // When only the layer surface content changed (e.g. waybar clock tick)
    // but no window moved behind us, we reuse the cached blurred background.
    // This skips the most expensive GPU work (blit + 6 blur passes).
    const uint64_t currentGeneration = g_pGlobalState->getSceneGeneration(monitor.get());
    const auto activeWs = monitor->m_activeWorkspace;
    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha().isBeingAnimated() ||
                             (activeWs && activeWs->m_renderOffset->isBeingAnimated());
    // [technicolor fork] Force a fresh re-sample for glass LAYER surfaces every
    // frame. The cache below assumes the backdrop only changes when a window
    // moves behind us or the layer itself animates — which is FALSE under an
    // animated/live wallpaper, so the alt-tab pie froze the wallpaper it saw at
    // map time. Only whitelisted namespaces become glass layers (here just
    // quickshell:pie), and this path only runs while such a layer is on screen,
    // so the extra per-frame sample+blur is negligible.
    const bool kForceLiveLayer = true;
    const bool backgroundChanged = kForceLiveLayer || !m_hasCachedSample ||
                                   currentGeneration != m_lastSceneGeneration ||
                                   isAnimating;

    if (!layerSurface->m_mapped) {   // 0.56: see above
        // During fade-out, re-sampling captures stale pixels. Reuse cached sample.
        if (!m_hasCachedSample)
            return;
    } else if (backgroundChanged) {
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

        // Layers have no window class to test against: when the terminal-only
        // restriction is on, overlays simply lose the adaptive tint.
        const bool termOnly = g_pGlobalState->config.adaptiveTintTerminalsOnly &&
                              **g_pGlobalState->config.adaptiveTintTerminalsOnly;
        if (!termOnly)
            GlassRenderer::updateAdaptiveLuma(m_sampleFramebuffer, m_lumaFb, m_lumaCurrent, m_lumaSeeded,
                                              dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(),
                                              viewportWidth, viewportHeight);

        m_hasCachedSample      = true;
        m_lastSceneGeneration  = currentGeneration;
    }
    // else: background unchanged, reuse cached blur — skip 7 GPU operations

    // Redirect surface rendering to a temp FBO cleared to transparent.
    // The original renderLayer (called between pre/post elements) will render
    // the surface into this FBO. compositeAndRestore uses its alpha as a mask.
    int monitorWidth  = static_cast<int>(monitor->m_transformedSize.x);
    int monitorHeight = static_cast<int>(monitor->m_transformedSize.y);

    // Force ARGB8888 for the temp FBO: the mask shader needs alpha precision.
    // Monitor FBOs use XRGB formats (no usable alpha); XRGB2101010 (10-bit)
    // has only 2-bit alpha, quantizing values below ~0.17 to zero and breaking
    // the mask discard for low-opacity layers.
    if (!m_surfaceTempFramebuffer)
        m_surfaceTempFramebuffer = g_pHyprRenderer->createFB("hyprwater-layer-temp");

    if (m_surfaceTempFramebuffer->m_size.x != monitorWidth || m_surfaceTempFramebuffer->m_size.y != monitorHeight)
        m_surfaceTempFramebuffer->alloc(monitorWidth, monitorHeight, DRM_FORMAT_ARGB8888);

    m_savedCurrentFB = source;

    g_pHyprRenderer->m_renderData.currentFB = m_surfaceTempFramebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_surfaceTempFramebuffer.get())->getFBID());

    CBox clearBox = transformBox;
    clearBox.expand(GlassRenderer::SAMPLE_PADDING_PX);
    clearBox = clearBox.intersection(CBox{0.0, 0.0, static_cast<double>(monitorWidth), static_cast<double>(monitorHeight)}).noNegativeSize().round();

    if (std::isfinite(clearBox.x) && std::isfinite(clearBox.y) && std::isfinite(clearBox.w) && std::isfinite(clearBox.h) &&
        clearBox.w > 0.0 && clearBox.h > 0.0) {
        g_pHyprOpenGL->scissor(clearBox, false);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprOpenGL->scissor(nullptr);
    }
}

void CGlassLayerSurface::compositeAndRestore(PHLMONITOR monitor, float alpha) {
    // Restore the original currentFB before compositing
    if (m_savedCurrentFB) {
        g_pHyprRenderer->m_renderData.currentFB = m_savedCurrentFB;
        glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_savedCurrentFB.get())->getFBID());
        m_savedCurrentFB.reset();
    }

    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized() || !m_hasCachedSample)
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto target = g_pHyprRenderer->m_renderData.currentFB;
    if (!target)
        return;

    auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return;

    CBox rawBox       = *layerBox;
    CBox transformBox = transformedLayerBox(rawBox, monitor);

    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float cornerRadius  = 0.0f;
    float roundingPower = 2.0f;

    // Use the temp FBO's rendered alpha as a mask: glass only where the surface
    // has visible content (alpha > 0). The temp FBO is in monitor coordinates,
    // so we map from the glass quad UV to monitor UV.
    int monitorWidth  = static_cast<int>(monitor->m_transformedSize.x);
    int monitorHeight = static_cast<int>(monitor->m_transformedSize.y);

    float maskThreshold = 0.001f;
    auto threshIt = g_pGlobalState->layerNamespaceMaskThresholds.find(layerSurface->m_namespace);
    if (threshIt != g_pGlobalState->layerNamespaceMaskThresholds.end())
        maskThreshold = threshIt->second;

    // The temp FBO stores the layer after Hyprland applies fade alpha. Keep
    // mask_threshold relative to the layer's content alpha, otherwise fade-out
    // makes the mask fall below threshold early and the glass blinks off.
    maskThreshold *= std::clamp(alpha, 0.0f, 1.0f);

    GlassRenderer::SMaskInfo maskInfo{
        .textureId         = m_surfaceTempFramebuffer->getTexture()->m_texID,
        .target            = GL_TEXTURE_2D,
        .uvOffset          = {transformBox.x / monitorWidth, transformBox.y / monitorHeight},
        .uvScale           = {transformBox.w / monitorWidth, transformBox.h / monitorHeight},
        .alphaThreshold    = maskThreshold,
    };

    // The glass shader composites both the glass effect and the surface content
    // in a single pass: glass behind, surface on top, using the temp FBO alpha.
    // A null luma framebuffer disables the adaptive tint for this pass.
    const bool termOnly = g_pGlobalState->config.adaptiveTintTerminalsOnly &&
                          **g_pGlobalState->config.adaptiveTintTerminalsOnly;
    SP<Render::IFramebuffer> lumaFb = termOnly ? nullptr : m_lumaFb[m_lumaCurrent];
    GlassRenderer::applyGlassEffect(m_sampleFramebuffer, target,
                                     rawBox, transformBox, alpha,
                                     cornerRadius, roundingPower, m_samplePaddingRatio, ctx,
                                     &maskInfo, lumaFb);
}
