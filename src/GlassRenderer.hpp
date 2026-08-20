#pragma once

#include "PluginConfig.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

// Shared GL rendering pipeline used by both window decorations and layer surfaces.
// Callers own their sample framebuffers; these functions operate on passed-in state.
namespace GlassRenderer {

void stepWaveSim();
void renderTrailTex();
void queueClickSplash();

// Temporary tick-hunt instrumentation (no-op unless the debug log is armed).
void DBG_LOG(const char* fmt, ...);

inline constexpr int SAMPLE_PADDING_PX = 60;

// Maximum downscale factor for blur sampling. Half-res (2) is 4x cheaper
// per blur pass. Only applied when blur is strong enough to hide the lower
// resolution — weak blur at half-res shows visible pixelation.
inline constexpr int   BLUR_DOWNSCALE_MAX       = 2;
inline constexpr float BLUR_DOWNSCALE_THRESHOLD = 0.35f; // min blur_strength for downscale
inline constexpr float BLUR_DOWNSCALE_QUARTER   = 1.30f; // min blur_strength for quarter-res

// How far down to sample the backdrop before blurring it.
//
// Screen-space reach is blur_strength * 12px whatever this returns, because the
// radius is divided by it — downscaling doesn't weaken the blur, it buys the
// SAME blur for a quarter of the work per step. What it does change is the
// ceiling: the shader caps its taps at 16, so the reachable radius is
// 16 * downscale, and at half-res that ceiling lands at blur_strength 2.67.
// Sampling at quarter-res doubles the headroom to 5.33 AND costs 4x less.
//
// The floor matters too: at low strength the blur is too tight to hide the
// lower resolution and you see the backdrop pixelate, so each step down only
// engages once the blur is wide enough to cover for it.
[[nodiscard]] inline int blurDownscale(float strength) {
    // Quarter-res is safe again: sampleBackground() now reduces 4x as TWO
    // sequential 2x blits, and GL_LINEAR's 2x2 average is the exact box
    // filter for a 2x step. A single 4x GL_LINEAR blit was not — it read a
    // 2x2 box out of a 4x4 footprint, discarding 12 of every 16 source
    // pixels. Undersampled text aliased into axis-aligned moire that
    // crawled with the window, because the sample phase is srcX0 = box.x-pad.
    if (strength >= BLUR_DOWNSCALE_QUARTER)  return BLUR_DOWNSCALE_MAX * 2;
    if (strength >= BLUR_DOWNSCALE_THRESHOLD) return BLUR_DOWNSCALE_MAX;
    return 1;
}

// Layers only: alpha mask from the temp FBO that captured the rendered surface.
// Constrains the glass effect to regions where the layer has visible content.
// Windows do not use masking, they pass mask=nullptr to applyGlassEffect.
struct SMaskInfo {
    GLuint   textureId;
    GLenum   target;
    Vector2D uvOffset; // mapping from glass box UV → full surface UV
    Vector2D uvScale;
    float    alphaThreshold = 0.001f;
};

void sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int downscale = 1);

// tempFramebuffer is the caller's OWN ping-pong scratch. It must not be shared
// between callers: it is sized to the sample being blurred, so a shared one
// thrashes between differently-sized windows and only the one whose size it
// currently holds gets a correct blur.
void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer>& tempFramebuffer,
                    float radius, int iterations,
                    GLuint callerFramebufferID, int viewportWidth, int viewportHeight);

// Reduce the blurred sample to ONE time-smoothed average luminance in a 1x1 FB.
// Ping-ponged because a fragment shader cannot carry state between frames, and
// an animated wallpaper would otherwise make the dim pump with the animation.
void updateAdaptiveLuma(SP<Render::IFramebuffer>& sampleFramebuffer,
                        SP<Render::IFramebuffer> lumaFb[2], int& current, bool& seeded,
                        GLuint callerFramebufferID, int viewportWidth, int viewportHeight);


// When mask is non-null (layers only), the shader composites the surface content
// over the glass effect in a single pass. When mask is null (windows), the shader
// outputs the glass effect alone.
void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, float cornerRadius, float roundingPower,
                       const Vector2D& paddingRatio, const SResolveContext& resolveContext,
                       const SMaskInfo* mask = nullptr,
                       SP<Render::IFramebuffer> adaptiveLumaFb = nullptr);

} // namespace GlassRenderer
