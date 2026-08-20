#include "GlassRenderer.hpp"
#include "BuiltInPresets.hpp"
#include "Globals.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <drm_fourcc.h>
#include <cstdarg>
#include <unistd.h>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace GlassRenderer {

// TEMPORARY diagnostic logging (enabled by HYPRWATER_DEBUG_LOG in the
// environment): timestamps of glass renders, window positions, sim steps and
// trail renders, to correlate against frame captures. Remove after the
// low-speed tick hunt.
static FILE* dbgLog() {
    static FILE* f = [] {
        if (const char* p = getenv("HYPRWATER_DEBUG_LOG"))
            return fopen(p, "w");
        // A live session cannot be handed a new environment variable, so a
        // trigger file works too: touch /tmp/hyprwater-debug, reload the
        // plugin, read /tmp/hyprwater-debug.log.
        if (access("/tmp/hyprwater-debug", F_OK) == 0)
            return fopen("/tmp/hyprwater-debug.log", "w");
        return static_cast<FILE*>(nullptr);
    }();
    return f;
}
static double dbgNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
#define DBG(...) do { if (FILE* _f = dbgLog()) { fprintf(_f, __VA_ARGS__); fflush(_f); } } while (0)

// Same sink, callable from the other translation units (timestamp prefixed).
void DBG_LOG(const char* fmt, ...) {
    FILE* f = dbgLog();
    if (!f)
        return;
    fprintf(f, "%.4f ", dbgNow());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

// Deterministic-A/B state (see HYPRWATER_FREEZE_AT in stepWaveSim).
static const long g_freezeAt = [] {
    const char* p = getenv("HYPRWATER_FREEZE_AT");
    return p ? atol(p) : 0L;
}();
static bool g_frozen = false;
static int  g_pendingFluidTicks = 0;
static bool g_harnessArmed = false;
static bool g_harnessClear = false;

static GLuint fbId(const SP<Render::IFramebuffer>& framebuffer) {
    // dynamic_cast returns null for anything that is not a GL framebuffer, and
    // calling ->getFBID() on that jumps through a garbage vtable — which shows
    // up in a crash report as a bare unresolved address with no symbol.
    if (!framebuffer)
        return 0;
    auto* gl = dynamic_cast<Render::GL::CGLFramebuffer*>(framebuffer.get());
    return gl ? gl->getFBID() : 0;
}

static void uploadThemeUniforms(const SResolveContext& ctx) {
    const auto& uniforms = g_pGlobalState->shaderManager.glassUniforms;
    const auto& defaults = ctx.isDark ? DARK_THEME_DEFAULTS : LIGHT_THEME_DEFAULTS;

    // Raw locations for ALL of these, deliberately. They used to go through
    // the compositor's shader-slot mechanism (setUniformFloat(SHADER_*)),
    // which has its own caching layer between the call and the GL program --
    // one more owner of when a value actually lands. Every other uniform in
    // this plugin is a direct glUniform to a location grabbed at compile;
    // these are now the same, so a draw's effective state is exactly what
    // the code just uploaded, with no second mechanism to disagree with it.
    glUniform1f(uniforms.brightness, resolvePresetFloat(ctx, &SPresetValues::brightness, &SOverridableConfig::brightness, defaults.brightness));
    glUniform1f(uniforms.contrast,   resolvePresetFloat(ctx, &SPresetValues::contrast, &SOverridableConfig::contrast, defaults.contrast));
    glUniform1f(uniforms.saturation, resolvePresetFloat(ctx, &SPresetValues::saturation, &SOverridableConfig::saturation, defaults.saturation));
    glUniform1f(uniforms.vibrancy,   resolvePresetFloat(ctx, &SPresetValues::vibrancy, &SOverridableConfig::vibrancy, defaults.vibrancy));
    glUniform1f(uniforms.vibrancyDarkness, resolvePresetFloat(ctx, &SPresetValues::vibrancyDarkness, &SOverridableConfig::vibrancyDarkness, defaults.vibrancyDarkness));

    glUniform1f(uniforms.adaptiveDim,   resolvePresetFloat(ctx, &SPresetValues::adaptiveDim, &SOverridableConfig::adaptiveDim, defaults.adaptiveDim));
    glUniform1f(uniforms.adaptiveBoost, resolvePresetFloat(ctx, &SPresetValues::adaptiveBoost, &SOverridableConfig::adaptiveBoost, defaults.adaptiveBoost));
}

void sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int downscale) {
    if (!sourceFramebuffer)
        return;
    const int pad = SAMPLE_PADDING_PX;
    int fullWidth  = static_cast<int>(box.width) + 2 * pad;
    int fullHeight = static_cast<int>(box.height) + 2 * pad;

    // Allocate sample FBO at reduced resolution when blur is strong enough
    // to hide the lower resolution. Weak blur at half-res shows pixelation.
    int sampleWidth  = std::max(1, fullWidth / downscale);
    int sampleHeight = std::max(1, fullHeight / downscale);

    if (!sampleFramebuffer)
        sampleFramebuffer = g_pHyprRenderer->createFB("hyprwater-sample");

    if (sampleFramebuffer->m_size.x != sampleWidth || sampleFramebuffer->m_size.y != sampleHeight)
// 16-BIT INTERMEDIATES. The blur ping-pongs between this FB and the temp one
// several times, and an 8-bit target rounds every pass. Blurring is exactly
// what turns detailed content into smooth low-contrast gradients, and those
// are what 8 bits cannot hold: adjacent levels are 1/255 apart, so a slow
// gradient sits on one level for many pixels and then steps. Repeating that
// per pass bakes visible contour stripes into the backdrop before anything
// downstream can help - which is why dithering the final write alone did
// nothing. Keeping every intermediate in half float leaves the only
// quantisation at the final composite, where the dither can do its job.
        sampleFramebuffer->alloc(sampleWidth, sampleHeight, DRM_FORMAT_ABGR16161616F);
        if (!sampleFramebuffer->getTexture() || fbId(sampleFramebuffer) == 0)
            sampleFramebuffer->alloc(sampleWidth, sampleHeight, sourceFramebuffer->m_drmFormat);

    int srcX0 = static_cast<int>(box.x) - pad;
    int srcX1 = static_cast<int>(box.x + box.width) + pad;
    int srcY0 = static_cast<int>(box.y) - pad;
    int srcY1 = static_cast<int>(box.y + box.height) + pad;

    // Clamp source coordinates to framebuffer bounds to avoid reading black/undefined pixels
    int framebufferWidth  = static_cast<int>(sourceFramebuffer->m_size.x);
    int framebufferHeight = static_cast<int>(sourceFramebuffer->m_size.y);

    // Destination coords in downscaled FBO space
    int dstX0 = 0, dstY0 = 0, dstX1 = sampleWidth, dstY1 = sampleHeight;

    // Scale destination adjustments proportionally for the downscaled FBO
    const float xScale = static_cast<float>(sampleWidth) / fullWidth;
    const float yScale = static_cast<float>(sampleHeight) / fullHeight;

    if (srcX0 < 0) { dstX0 += static_cast<int>(-srcX0 * xScale); srcX0 = 0; }
    if (srcY0 < 0) { dstY0 += static_cast<int>(-srcY0 * yScale); srcY0 = 0; }
    if (srcX1 > framebufferWidth)  { dstX1 -= static_cast<int>((srcX1 - framebufferWidth) * xScale);  srcX1 = framebufferWidth; }
    if (srcY1 > framebufferHeight) { dstY1 -= static_cast<int>((srcY1 - framebufferHeight) * yScale); srcY1 = framebufferHeight; }

    // Padding ratio is relative to the logical content area (resolution-independent)
    outPaddingRatio = Vector2D(
        static_cast<double>(pad) / fullWidth,
        static_cast<double>(pad) / fullHeight
    );

    // The render pass scissors each element to its damage region.
    // That scissor state leaks here and clips glBlitFramebuffer on the
    // DRAW framebuffer, causing partial writes and stale noise artifacts.
    // RAW disable, not setCapStatus: the compositor's cap cache can believe
    // the scissor is already off while the GL state says on - the cached
    // wrapper then skips the glDisable and the clear and blit stay clipped
    // to the damage rect. Regions outside it keep their old texels forever;
    // when VRAM history put something bright there, every window sampling
    // this FB strobed. Query-and-restore around the whole copy.
    const GLboolean smpScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

    // Clear the sample FBO before blitting. Clamped regions (near edges)
    // would otherwise contain uninitialized GPU memory (pink artifacts).
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // TWO-STEP REDUCTION. GL_LINEAR averages exactly 2x2 texels, which is the
    // correct box filter for a 2x reduction and NOT correct for anything larger:
    // a single 4x blit reads a 2x2 box out of a 4x4 footprint and discards 12 of
    // every 16 source pixels. Undersampled text aliases into axis-aligned moire,
    // and since the sample phase is srcX0 = box.x - pad, that moire crawls
    // whenever the window moves and freezes the moment it stops -- which is
    // exactly how the artifact presented. Composing two exact 2x steps gives a
    // correct 4x4 average at the same final resolution, so quarter-res keeps its
    // fill-rate saving without the aliasing.
    if (downscale >= 4) {
        static SP<Render::IFramebuffer> halfFramebuffer;
        const int halfWidth  = std::max(1, sampleWidth  * 2);
        const int halfHeight = std::max(1, sampleHeight * 2);
        if (!halfFramebuffer)
            halfFramebuffer = g_pHyprRenderer->createFB("hyprwater-sample-half");
        if (halfFramebuffer->m_size.x != halfWidth || halfFramebuffer->m_size.y != halfHeight)
            halfFramebuffer->alloc(halfWidth, halfHeight, DRM_FORMAT_ABGR16161616F);
        if (!halfFramebuffer->getTexture() || fbId(halfFramebuffer) == 0)
            halfFramebuffer->alloc(halfWidth, halfHeight, sourceFramebuffer->m_drmFormat);

        // Step 1: source -> half res. The destination rect is the final one
        // scaled by two, so this step is exactly 2x and the clipping math above
        // carries over unchanged.
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(halfFramebuffer));
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(halfFramebuffer));
        glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
                          dstX0 * 2, dstY0 * 2, dstX1 * 2, dstY1 * 2,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

        // Step 2: half res -> final. Whole surface to whole surface, so this is
        // exactly 2x again and the content keeps its position.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(halfFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
        glBlitFramebuffer(0, 0, halfWidth, halfHeight,
                          0, 0, sampleWidth, sampleHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
        glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
                          dstX0, dstY0, dstX1, dstY1,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (smpScissor)
        glEnable(GL_SCISSOR_TEST);
}

// ============================================================================
//  WAVE SIMULATION STEP
//
//  One explicit integration of d2h/dt2 = c^2*lap(h) per frame, ping-ponged
//  between two framebuffers. This is the state that makes the surface a
//  DYNAMICAL SYSTEM rather than a formula: a disturbance spreads at finite
//  speed, bounces off the edges, interferes with its own reflections, and dies
//  out. Sums of sines cannot do any of that, which is why every analytic
//  variant read as "patterned" no matter how it was tuned.
//
//  Small grid on purpose: 512x512 costs ~260k texels x 5 taps once per frame,
//  which is nothing beside the caustic pass running over every glass pixel.
// ============================================================================
// Push a finished stroke segment into the pending ring. A FULL ring REFUSES
// the push (returns false) and the caller keeps extending its live segment
// instead: a lengthening chord is CONTINUOUS, so trail fidelity degrades
// smoothly under saturation. The earlier policy merged the two oldest
// segments into a chord — a discrete geometry snap that fired many times a
// second during a hard whip at ultra-low sim speed (one absorption step per
// SECOND at speed 0.002), which was exactly the residual jitter the user
// kept seeing on window and mouse drags while clicks stayed smooth.
static bool pushPendStroke(const SGlobalState::SDrag& s) {
    auto& st = *g_pGlobalState;
    if (st.pendLen == SGlobalState::PEND_RING)
        return false;
    st.pendRing[(st.pendHead + st.pendLen) % SGlobalState::PEND_RING] = s;
    st.pendLen++;
    return true;
}

// Shared by the wave step and the fluid passes: draw one quad over the whole
// target, no window geometry involved.
static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
    2.0f, 0.0f, 0.0f,
    0.0f, 2.0f, 0.0f,
   -1.0f,-1.0f, 1.0f,
};

// ============================================================================
//  CURRENTS — one Stable Fluids step (Stam 1999): advect, force, project.
//
//  The wave sim above this is a SCALAR height field; curl of a scalar is
//  undefined, so it structurally cannot hold an eddy — the user's whirlpools
//  need an actual velocity field. This maintains one at low resolution
//  (velocity fields are smooth; the projection smooths them further) and the
//  wave step samples it bilinearly, so currents bend and carry the waves.
//
//  Deliberately NOT moving solid boundaries: window edges inject FORCE into
//  one shared sheet (they are permeable — windows contain nothing, overlapping
//  windows are two viewports onto the same fluid). A force can move water but
//  never create it; the pressure projection enforcing div v = 0 IS the mass
//  conservation that makes this safe. Real moving walls would need solid-fluid
//  coupling and can trap and compress fluid — that case is intentionally out.
//
//  Cost: (2 + 24 + 1) draws over 256^2 per step ≈ 2M texel updates, against a
//  caustic pass doing ~18 BILLION reads/sec. Invisible. Verified against a
//  numpy prototype first: vortex dipole forms behind a drag, a wave packet is
//  carried at the current's speed, and 4000 steps stay bounded.
// ============================================================================
static constexpr int SIM          = 1024;   // height-field grid

// Grid size comes from shimmer:currents_resolution (default 512). At 512 the
// desktop spans ~107 fluid texels, a typical window ~27 — swirls small enough
// to read as corner turbulence rather than one window-sized smear. The FBO
// alloc below compares against this every step, so changing the key live just
// reallocates and restarts the field from still water.
static int fluidRes() {
    const auto& cfg = g_pGlobalState->config;
    const int64_t r = cfg.shimmerCurrentsRes ? **cfg.shimmerCurrentsRes : 512;
    return static_cast<int>(std::clamp<int64_t>(r, 64, 2048));
}

static void bindSimTexture(int unit, const SP<Render::IFramebuffer>& fb) {
    // Explicit filtering EVERY bind, same lesson as the wave texture: nothing
    // else sets these, and NEAREST prints the grid. CLAMP keeps the boundary
    // from wrapping to the far side. LINEAR is exact at texel centers, so the
    // whole-texel offsets of the derivative stencils are unaffected.
    glActiveTexture(GL_TEXTURE0 + unit);
    fb->getTexture()->bind();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// THE FLUID RUNS CONTINUOUSLY, DECOUPLED FROM THE WAVE STEPS. It used to
// advance once per WAVE step, which at low sim speed meant a whole second of
// whip momentum landed in the field as one lump: the water's apparent motion
// surged in step-rate beats even though every frame rendered on time — the
// low-speed "tick" that survived every frame-timing check. Now the fluid
// integrates a micro-step of simulated time (elapsed real time × speed) at a
// fixed real-time cadence, so momentum enters the frame it happens and the
// velocity the glass reads never jumps. Cost is FLAT versus sim speed:
// 9 passes per tick instead of 27 per wave step (which at normal speeds was
// 27 × 48..480 steps/s).
//
// Numerical note: more ticks per second means more semi-Lagrangian resampling
// smear, which is why the cadence is ~33 Hz and not the frame rate — the old
// code at NORMAL speeds ran 48-480 resamples/s and the eddies looked right,
// so 33/s is safely inside the proven regime. 33 Hz also puts force arrival
// at 30 ms granularity, far below anything the eye reads as a beat.
static constexpr int    FLUID_JACOBI_TICK = 4;      // warm-started: ~130 iters/s
static constexpr double FLUID_TICK_S      = 0.030;  // ~33 Hz ceiling
// Simulated seconds that must accrue before a FORCE-FREE tick is worth doing.
// Below this the advection offset is a fraction of a texel (velocities run
// ~0.01-0.6 uv/s, a 2048 texel is 4.9e-4 uv) — i.e. the pass would resample
// the field onto itself and change nothing, so idle water at low speed simply
// waits. This is what keeps the continuous tick CHEAPER than the old
// per-wave-step burst rather than more expensive.
static constexpr double FLUID_IDLE_SIM_S  = 2.0e-4;

static int SUBof(double sp) { return sp >= 0.4 ? 1 : (sp >= 0.15 ? 2 : 4); }

static void stepFluidFrame() {
    if (!g_pGlobalState)
        return;
    auto& st = *g_pGlobalState;
    const auto& cfg = st.config;

    const bool currentsOn = cfg.shimmerCurrents && **cfg.shimmerCurrents != 0;
    const double speed = cfg.shimmerSpeed
        ? std::clamp(static_cast<double>(**cfg.shimmerSpeed), 0.0, 4.0) : 1.0;
    // Same fp16 requirement as the wave field: velocity is SIGNED and small.
    // The wave alloc already decided the format; follow it.
    const bool fp16 = st.waveFb[0] && st.waveFb[0]->m_drmFormat == DRM_FORMAT_ABGR16161616F;
    static bool currentsWere = false;
    static double pendingSim = 0.0;
    if (!currentsOn || !fp16 || speed <= 0.0) {
        st.flowDt = 0.0f;
        currentsWere = false;   // so re-enabling starts from still water
        pendingSim = 0.0;
        return;
    }

    // The glass shader's advected interpolation slides by ONE WAVE STEP's
    // advection (that is what separates the two sim states it blends).
    // Published before the cadence gates below so the glass never loses the
    // field on a frame this function returns early from.
    const int SUB = speed >= 0.4 ? 1 : (speed >= 0.15 ? 2 : 4);
    st.flowDt = 1.0f / (120.0f * SUB);

    // At most one tick per 30 ms, however many glassed surfaces call in —
    // except under the deterministic harness, where the fluid advances exactly
    // once per wave step so it cannot depend on wall clock either.
    double elapsed;
    if (g_freezeAt > 0) {
        if (g_pendingFluidTicks <= 0)
            return;
        g_pendingFluidTicks--;
        elapsed = 1.0 / (120.0 * SUBof(speed));
    } else {
        static std::chrono::steady_clock::time_point lastTick{};
        const auto now = std::chrono::steady_clock::now();
        elapsed = std::chrono::duration<double>(now - lastTick).count();
        if (elapsed < FLUID_TICK_S)
            return;
        lastTick = now;
    }

    // Simulated time waiting to be integrated. Capped so a stall integrates a
    // bounded amount instead of replaying itself as one big lurch. Time keeps
    // ACCUMULATING across skipped ticks, so the dissipation and advection a
    // skipped tick would have applied are not lost, just deferred.
    pendingSim += std::min(elapsed, 0.25) * speed;
    const bool haveForce = st.fluidForceWin.amount > 1e-6f
                        || st.fluidForceMouse.amount > 1e-6f;
    if (!haveForce && pendingSim < FLUID_IDLE_SIM_S)
        return;
    const float dt = static_cast<float>(pendingSim);
    pendingSim = 0.0;

    // Projection is only needed when new momentum arrived: a divergence-free
    // field advected by itself stays (to first order) divergence-free, so an
    // unforced tick is just advect + dissipate — one pass instead of seven.
    // The periodic cleanup mops up the second-order divergence semi-Lagrangian
    // advection does introduce. Idle water therefore costs LESS than the old
    // per-wave-step burst did, and the full solve is spent where it is
    // actually visible: while something is stirring the pool.
    static int cleanupCountdown = 0;
    const bool doProject = haveForce || --cleanupCountdown <= 0;
    if (doProject)
        cleanupCountdown = 8;

    // Save the caller's target FIRST: the housekeeping below binds and clears
    // fluid FBOs, and saving after it would "restore" the caller onto one of
    // them — every glass draw after that would land in the fluid texture.
    GLint prevFbo = 0, prevVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);

    // FBO housekeeping (moved from the wave step so it happens even on frames
    // where the slow sim does not advance). Changing the resolution key live
    // just reallocates and restarts the field from still water.
    const int res = fluidRes();
    {
        bool freshFluid = !currentsWere;   // stale field from before the toggle
        currentsWere = true;
        SP<Render::IFramebuffer>* fbs[] = {&st.fluidVelFb[0], &st.fluidVelFb[1],
                                           &st.fluidPrsFb[0], &st.fluidPrsFb[1],
                                           &st.fluidDivFb};
        const char* names[] = {"hyprwater-vel-a", "hyprwater-vel-b",
                               "hyprwater-prs-a", "hyprwater-prs-b",
                               "hyprwater-div"};
        for (size_t i = 0; i < std::size(names); i++) {
            auto& fb = *fbs[i];
            if (!fb) {
                DBG("%.4f FLUIDHK create %s\n", dbgNow(), names[i]);
                fb = g_pHyprRenderer->createFB(names[i]);
            }
            if (!fb) {
                st.flowDt = 0.0f;
                return;
            }
            if (fb->m_size.x != res || fb->m_size.y != res) {
                DBG("%.4f FLUIDHK alloc %s %d\n", dbgNow(), names[i], res);
                fb->alloc(res, res, DRM_FORMAT_ABGR16161616F);
                freshFluid = true;
            }
            if (!fb->getTexture() || fbId(fb) == 0) {
                st.flowDt = 0.0f;
                return;
            }
        }
        if (freshFluid) {
            // Zero is genuinely zero here (still water, no pressure), unlike
            // the height textures whose flat state is the bias value.
            for (auto* fb : fbs) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbId(*fb));
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            DBG("%.4f FLUIDHK cleared\n", dbgNow());
        }
    }

    const auto& fu = st.shaderManager.fluidUniforms;
    const float texel = 1.0f / res;

    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
    g_pHyprOpenGL->setViewport(0, 0, res, res);

    // One force slot per tick; the window's momentum outranks the fingertip's.
    // Whichever is skipped keeps its accumulation for the next tick (28 ms
    // away, not a wave step away).
    auto& fw = st.fluidForceWin;
    auto& fm = st.fluidForceMouse;
    auto& dragNow = fw.amount > 1e-6f ? fw : fm;
    const float spentForce = dragNow.amount;

    // 1) Advect the velocity by itself + inject the momentum + bleed.
    {
        auto sh = g_pHyprOpenGL->useShader(st.shaderManager.fluidAdvectShader);
        sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        sh->setUniformInt(SHADER_TEX, 3);
        glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
        glUniform1f(fu.aDt, dt);
        // ~4 s momentum half-life on top of what the projection and the
        // semi-Lagrangian interpolation already dissipate: eddies outlive the
        // drag that made them by a few seconds, then the water goes still.
        // Retention follows dt so the half-life is a property of simulated
        // time, not of how finely it is sliced.
        glUniform1f(fu.aDissipation, std::pow(0.998f, dt * 120.0f));
        if (dragNow.amount > 1e-6f) {
            // The force shader adds the splat WITHOUT a dt factor, so the
            // per-second momentum total is the same whether it arrives as one
            // wave-step lump (the old way) or as ~35 small spends (now) —
            // the accumulators integrate distance either way.
            glUniform2f(fu.aForceDir, dragNow.dx, dragNow.dy);
            glUniform4f(fu.aForce, dragNow.x, dragNow.y,
                        dragNow.r > 0.0f ? dragNow.r : 0.045f,
                        dragNow.amount * 12.0f);
            dragNow.amount = 0.0f;
        } else {
            glUniform2f(fu.aForceDir, 0.0f, 0.0f);
            glUniform4f(fu.aForce, 0.0f, 0.0f, 1.0f, 0.0f);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(st.fluidVelFb[1 - st.fluidVelCurrent]));
        bindSimTexture(3, st.fluidVelFb[st.fluidVelCurrent]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        st.fluidVelCurrent = 1 - st.fluidVelCurrent;
    }

    // 2) How much each cell is now being pumped or drained.
    if (doProject) {
        auto sh = g_pHyprOpenGL->useShader(st.shaderManager.fluidDivergenceShader);
        sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        sh->setUniformInt(SHADER_TEX, 3);
        glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
        glUniform2f(fu.dTexelSize, texel, texel);
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(st.fluidDivFb));
        bindSimTexture(3, st.fluidVelFb[st.fluidVelCurrent]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // 3) Relax lap(q) = div. q is warm-started ACROSS ticks, so at ~35 Hz the
    //    4 iterations here compound into ~130/s — more relaxation per second
    //    than the old 24-per-wave-step ever delivered at low speed.
    if (doProject) {
        auto sh = g_pHyprOpenGL->useShader(st.shaderManager.fluidJacobiShader);
        sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        sh->setUniformInt(SHADER_TEX, 3);
        glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
        glUniform2f(fu.jTexelSize, texel, texel);
        glUniform1i(fu.jDivTex, 4);
        bindSimTexture(4, st.fluidDivFb);
        for (int j = 0; j < FLUID_JACOBI_TICK; j++) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbId(st.fluidPrsFb[1 - st.fluidPrsCurrent]));
            bindSimTexture(3, st.fluidPrsFb[st.fluidPrsCurrent]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            st.fluidPrsCurrent = 1 - st.fluidPrsCurrent;
        }
    }

    // 4) Keep only the swirl.
    if (doProject) {
        auto sh = g_pHyprOpenGL->useShader(st.shaderManager.fluidGradientShader);
        sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        sh->setUniformInt(SHADER_TEX, 3);
        glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
        glUniform2f(fu.gTexelSize, texel, texel);
        glUniform1i(fu.gPrsTex, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(st.fluidVelFb[1 - st.fluidVelCurrent]));
        bindSimTexture(3, st.fluidVelFb[st.fluidVelCurrent]);
        bindSimTexture(4, st.fluidPrsFb[st.fluidPrsCurrent]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        st.fluidVelCurrent = 1 - st.fluidVelCurrent;
    }

    DBG("%.4f FLUIDTICK dt=%.5f force=%.4f proj=%d\n", dbgNow(), dt, spentForce,
        static_cast<int>(doProject));

    g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    g_pHyprOpenGL->setViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
}

// ---- Mouse wake ----------------------------------------------------------
// The cursor is a fingertip trailed in the pool: same distance-based
// accumulator as a dragged window, mapped through the same desktop-space
// formula so it disturbs the same sheet, but far lighter and far smaller.
// Sampled once per FRAME. It used to be sampled inside the wave step — at
// low sim speed that is once a SECOND: the wake became a 1 Hz polyline of
// ≤400 px chords, and any brisk flick tripped the teleport guard and vanished
// entirely. The cursor is real-time input; it must be sampled in real time.
static void sampleMouseWake() {
    if (!g_pGlobalState)
        return;
    // ~72 Hz, however many glassed surfaces and monitors call in. The
    // illumination is BLURRED by design (finite sun), so refresh-rate updates
    // buy nothing visible -- and this pass at full dual-monitor rate was
    // enough GPU to starve the compositor into flashing on the live desktop.
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() < 0.014)
        return;
    last = now;

    const auto& mcfg = g_pGlobalState->config;
    const float mforce = mcfg.shimmerMouse
                       ? std::clamp(static_cast<float>(**mcfg.shimmerMouse), 0.0f, 1.0f) : 0.0f;
    static Vector2D prevCur{-1e9, -1e9};
    const Vector2D cur = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : prevCur;
    const Vector2D d{cur.x - prevCur.x, cur.y - prevCur.y};
    prevCur = cur;
    const double mag = std::sqrt(d.x * d.x + d.y * d.y);
    // Ignore sub-pixel jitter and teleports (first sample, warps). Per-frame
    // deltas make 400 px a genuine teleport again, not just a fast flick.
    if (mforce > 0.001f && mag > 0.3 && mag < 400.0) {
        const auto& desk = g_pGlobalState->deskMax;
        const float sc = mcfg.shimmerScale
                       ? static_cast<float>(**mcfg.shimmerScale) : 1.0f;
        const Vector2D g{(cur.x - desk.x * 0.5) / std::max(desk.x, 1.0),
                         (cur.y - desk.y * 0.5) / std::max(desk.x, 1.0)};
        auto& mk = g_pGlobalState->mouse;
        const float mx = static_cast<float>(0.5 + g.x * 0.85 * sc * 2.0 * 0.105);
        const float my = static_cast<float>(0.5 + g.y * 0.85 * sc * 2.0 * 0.105);
        if (mk.amount <= 1e-6f) {
            const float k = static_cast<float>(0.85 * sc * 2.0 * 0.105 / std::max(desk.x, 1.0));
            mk.px = mx - static_cast<float>(d.x) * k;
            mk.py = my - static_cast<float>(d.y) * k;
        }
        mk.x  = mx;
        mk.y  = my;
        mk.dx = static_cast<float>(d.x / mag);
        mk.dy = static_cast<float>(d.y / mag);
        // A fingertip, not a hull: ~10 sim texels wide, and per-pixel
        // deposit about a third of a window's at full slider.
        mk.r  = 0.010f;
        mk.amount = std::min(mk.amount + static_cast<float>(mag) * 0.00012f * mforce,
                             0.012f * std::max(mforce, 0.05f));
        // The wake goes through the SAME ring and analytic trail as window
        // drags: same physics, same pipeline, at any sim speed.
        if (std::hypot(mk.x - mk.px, mk.y - mk.py) > 0.009f && pushPendStroke(mk)) {
            mk.px = mk.x;
            mk.py = mk.y;
            mk.amount = 0.0f;
        }
        // And its momentum feeds the continuous fluid tick.
        auto& ff = g_pGlobalState->fluidForceMouse;
        ff.x  = mx;  ff.y = my;
        ff.dx = mk.dx; ff.dy = mk.dy;
        ff.r  = mk.r;
        ff.amount = std::min(ff.amount + static_cast<float>(mag) * 0.00012f * mforce,
                             0.012f * std::max(mforce, 0.05f));
    }
}

// A click is a TAP on the surface: a small round press-in at the cursor,
// spent by the next simulation step. Strength rides the same mouse slider as
// the trailing wake — the weight of the fingertip governs both — so there is
// no separate toggle to find; slider at zero lifts the whole hand off.
// Called from the mouse-button listener in main.cpp on every press.
void queueClickSplash() {
    if (!g_pGlobalState)
        return;
    const auto& cfg = g_pGlobalState->config;
    const bool on = cfg.shimmerEnabled && **cfg.shimmerEnabled != 0;
    const float mforce = cfg.shimmerMouse
                       ? std::clamp(static_cast<float>(**cfg.shimmerMouse), 0.0f, 1.0f) : 0.0f;
    if (!on || mforce <= 0.001f || !g_pInputManager)
        return;
    const Vector2D cur  = g_pInputManager->getMouseCoordsInternal();
    const auto&    desk = g_pGlobalState->deskMax;
    const float    sc   = cfg.shimmerScale ? static_cast<float>(**cfg.shimmerScale) : 1.0f;
    const Vector2D g{(cur.x - desk.x * 0.5) / std::max(desk.x, 1.0),
                     (cur.y - desk.y * 0.5) / std::max(desk.x, 1.0)};
    auto& ck = g_pGlobalState->click;
    ck.x  = static_cast<float>(0.5 + g.x * 0.85 * sc * 2.0 * 0.105);
    ck.y  = static_cast<float>(0.5 + g.y * 0.85 * sc * 2.0 * 0.105);
    ck.dx = 0.0f;   // zero direction = the shader's ROUND splash, not a dipole
    ck.dy = 0.0f;
    // A touch wider than the wake radius: short wavelengths carry the square
    // grid's residual anisotropy (the faint 4-cornered ring the user spotted)
    // and a broader tap simply emits fewer of them.
    ck.r  = 0.022f;
    // Rapid clicks stack a little, capped: a drum-roll is a bigger splash,
    // not an unbounded one. Mostly slider-proportional with only a whisper of
    // a floor — the first cut had a fat constant base, which at a low slider
    // made every click dwarf the wake it was supposed to accompany.
    ck.amount = std::min(ck.amount + 0.012f + 0.06f * mforce, 0.20f);
}

// Sum the analytic stroke trail into its own texture, once per FRAME — the
// trail moves with the window every frame, unlike the sim. ~26 capsules per
// texel evaluated ONCE here beats evaluating them inside all ~26 stencil taps
// of every caustic pixel, and puts no ceiling on how finely a whip is traced.
void renderTrailTex() {
    auto& st = *g_pGlobalState;
    auto& sm = st.shaderManager;
    if (!sm.isInitialized())
        return;

    using namespace std::chrono;
    static steady_clock::time_point last{};
    const auto now = steady_clock::now();
    if (duration_cast<microseconds>(now - last).count() < 3000)
        return;   // several glassed surfaces per frame; render once
    last = now;

    if (!st.trailFb)
        st.trailFb = g_pHyprRenderer->createFB("hyprwater-trail");
    static uint32_t fmt = DRM_FORMAT_ABGR16161616F;
    if (st.trailFb->m_size.x != SIM || st.trailFb->m_size.y != SIM) {
        st.trailFb->alloc(SIM, SIM, fmt);
        if (!st.trailFb->getTexture() || fbId(st.trailFb) == 0) {
            // UNORM fallback loses the trough half (no negatives) — degraded
            // but functional, and matches the sim's own fallback policy.
            fmt = DRM_FORMAT_ABGR8888;
            st.trailFb->alloc(SIM, SIM, fmt);
        }
    }
    if (!st.trailFb->getTexture() || fbId(st.trailFb) == 0)
        return;

    GLint prevFbo = 0, prevVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);

    float seg[104] = {0}, par[104] = {0};
    int   n = 0;
    auto put = [&](const SGlobalState::SDrag& s, float wScale) {
        if (n >= 26 || s.amount <= 1e-5f || wScale <= 0.0f)
            return;
        const float ra  = s.r > 0.0f ? s.r : 0.045f;
        const float len = std::hypot(s.x - s.px, s.y - s.py);
        seg[n * 4 + 0] = s.px; seg[n * 4 + 1] = s.py;
        seg[n * 4 + 2] = s.x;  seg[n * 4 + 3] = s.y;
        par[n * 4 + 0] = s.dx; par[n * 4 + 1] = s.dy;
        par[n * 4 + 2] = ra;
        par[n * 4 + 3] = wScale * s.amount / (1.0f + 0.6f * len / ra);
        n++;
    };
    for (int i = 0; i < st.pendLen; i++)
        put(st.pendRing[(st.pendHead + i) % SGlobalState::PEND_RING], 1.0f);
    put(st.drag, 1.0f);
    put(st.mouse, 1.0f);   // the cursor's live stroke rides the trail too
    // The strokes absorbed by the current sim step fade here at exactly the
    // complement of the crossfade their texture copies are arriving with.
    for (int i = 0; i < st.lastAbsCount; i++)
        put(st.lastAbs[i], 1.0f - st.waveSubFrac);

    DBG("%.4f TRAIL n(pending)=%d subFrac=%.3f\n", dbgNow(), st.pendLen, st.waveSubFrac);
    auto sh = g_pHyprOpenGL->useShader(sm.trailShader);
    sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
    glUniform4fv(sm.trailUniforms.tSeg, 26, seg);
    glUniform4fv(sm.trailUniforms.tPar, 26, par);
    g_pHyprOpenGL->setViewport(0, 0, SIM, SIM);
    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(st.trailFb));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    g_pHyprOpenGL->setViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
}

// Render the caustic illumination into its own texture, once per FRAME, and
// blur it. Two reasons this cannot live in the glass shader any more.
//
// Physics: the sun is not a point. It is about half a degree wide, so every
// part of it casts its own slightly shifted caustic and the floor sees their
// sum. Without that blur a single bump shows BOTH of its astigmatic folds as
// separate rings - the doubling that appeared on every wave, click and drag.
// The blur is ~12 screen pixels; per-fragment that would be ~150 Hessian
// evaluations, here it is a separable Gaussian over the whole frame.
//
// Cost: per-screen-pixel this was ~12 texture reads for every pixel of every
// glassed window. Now it is ~12 reads per SIM TEXEL, once. On a wide desktop
// that is several times less work, not more.
// Resolution of the caustic pass, and also the splat grid: one point per
// target texel. The analytic version had to supersample because POINT-SAMPLING
// a sub-texel filament misses it entirely; a splat cannot miss -- every
// point's parcel lands somewhere -- so the native resolution keeps all the
// energy and the finite-sun blur below does the softening.
static constexpr int CAUSTIC_RES = 1024;

// ============================================================================
// BAND-LIMITED SURFACE FOR THE REFRACTION WARP
//
// The warp maps the backdrop p -> p + k*grad(h). That map shows the backdrop
// ONCE only while det(I + k*H) > 0 — the deciding quantity is the surface
// CURVATURE, not the displacement. Past that the map folds and the same patch
// of backdrop is drawn two or three times with a hard crease along the fold,
// which is exactly the doubled-wave artifact. Measured at the live settings
// (depth 3.5, scale 3) the map folded over 12.8% of every glass window even
// after the magnitude limiter, because that limiter is a function of the SLOPE
// and folds happen at crests where the slope is zero.
//
// Curvature scales as 1/lambda^2, so it lives almost entirely in the shortest
// wavelengths, while the displacement comes from the mid-scale swell. Low-pass
// the surface the warp reads and the folds go away for almost no loss of
// motion: sigma = 12 sim texels took the fold fraction 12.8% -> 0.0% (min det
// +0.31, provably one-to-one) while mean displacement only fell 31 px -> 19 px.
//
// Quarter resolution, so sigma 3 texels here is sigma 12 on the sim grid and
// the whole thing costs two blits and two 256^2 draws. The caustic keeps
// reading the SHARP field — folds there are the physics that draws the veins.
// ============================================================================
static constexpr int WAVE_SMOOTH_RES = SIM / 4;

static void buildSmoothWave() {
    auto& st  = *g_pGlobalState;
    auto& sm  = st.shaderManager;
    auto& src = st.waveFb[st.waveCurrent];
    if (!src || !src->getTexture() || fbId(src) == 0)
        return;

    // Half float regardless of what the wave field itself fell back to: this is
    // a SMOOTH field of ~0.01-amplitude values and 8 bits would quantise its
    // gradient into terraces. The stored bias (0.5 on the UNORM fallback) is
    // just a constant and survives a linear filter untouched, so mixing formats
    // is safe — the shader subtracts waveBias either way.
    auto ensure = [&](SP<Render::IFramebuffer>& f, const char* name, int n) {
        if (!f)
            f = g_pHyprRenderer->createFB(name);
        if (f->m_size.x != n || f->m_size.y != n) {
            f->alloc(n, n, DRM_FORMAT_ABGR16161616F);
            if (!f->getTexture() || fbId(f) == 0)
                f->alloc(n, n, src->m_drmFormat);
        }
        return f->getTexture() && fbId(f) != 0;
    };
    if (!ensure(st.waveHalfFb,      "hyprwater-wave-half",       SIM / 2))          return;
    if (!ensure(st.waveSmoothFb,    "hyprwater-wave-smooth",     WAVE_SMOOTH_RES))  return;
    if (!ensure(st.waveSmoothTmpFb, "hyprwater-wave-smooth-tmp", WAVE_SMOOTH_RES))  return;

    // TWO EXACT 2x STEPS, never one 4x blit. GL_LINEAR averages 2x2 texels and
    // nothing more, so a single 4x reduction reads a 2x2 box out of a 4x4
    // footprint and discards 12 of every 16 — the same aliasing the backdrop
    // sampler had to be fixed for.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(src));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(st.waveHalfFb));
    glBlitFramebuffer(0, 0, SIM, SIM, 0, 0, SIM / 2, SIM / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(st.waveHalfFb));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(st.waveSmoothFb));
    glBlitFramebuffer(0, 0, SIM / 2, SIM / 2, 0, 0, WAVE_SMOOTH_RES, WAVE_SMOOTH_RES,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Separable Gaussian. The blur shader takes a RADIUS and uses sigma =
    // radius/3, so 9 texels here is sigma 3 at quarter res == sigma 12 on the
    // sim grid, which is the measured figure above.
    const auto& bu = sm.blurUniforms;
    auto sh = g_pHyprOpenGL->useShader(sm.blurShader);
    sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    // Unit 8, same cache-poisoning rule as every other offscreen pass: the
    // compositor caches what it believes is bound on the low units.
    sh->setUniformInt(SHADER_TEX, 8);
    glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
    glUniform1f(bu.radius, 9.0f);
    g_pHyprOpenGL->setViewport(0, 0, WAVE_SMOOTH_RES, WAVE_SMOOTH_RES);
    glActiveTexture(GL_TEXTURE8);

    auto pass = [&](SP<Render::IFramebuffer>& from, SP<Render::IFramebuffer>& to, float dx, float dy) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(to));
        from->getTexture()->bind();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform2f(bu.direction, dx, dy);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };
    pass(st.waveSmoothFb, st.waveSmoothTmpFb, 1.0f / WAVE_SMOOTH_RES, 0.0f);
    pass(st.waveSmoothTmpFb, st.waveSmoothFb, 0.0f, 1.0f / WAVE_SMOOTH_RES);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}

static void renderCausticTex() {
    auto& st = *g_pGlobalState;
    auto& sm = st.shaderManager;
    if (!sm.isInitialized())
        return;

    const auto& cfg = st.config;
    const bool on = cfg.shimmerEnabled && **cfg.shimmerEnabled != 0
                 && cfg.shimmerIntensity && **cfg.shimmerIntensity > 0.0;
    if (!on)
        return;

    // ~72 Hz, however many glassed surfaces and monitors call in. The
    // illumination is BLURRED by design (finite sun), so refresh-rate updates
    // buy nothing visible -- and this pass at full dual-monitor rate was
    // enough GPU to starve the compositor into flashing on the live desktop.
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() < 0.014)
        return;
    last = now;

    auto& fb  = st.causticFb;
    auto& tmp = st.causticTmpFb;
    if (!fb)  fb  = g_pHyprRenderer->createFB("hyprwater-caustic");
    if (!tmp) tmp = g_pHyprRenderer->createFB("hyprwater-caustic-tmp");
    static uint32_t cfmt = DRM_FORMAT_ABGR16161616F;
    for (auto* f : {&fb, &tmp}) {
        if ((*f)->m_size.x != CAUSTIC_RES || (*f)->m_size.y != CAUSTIC_RES)
            (*f)->alloc(CAUSTIC_RES, CAUSTIC_RES, cfmt);
    }
    if (!fb->getTexture() || !tmp->getTexture() || fbId(fb) == 0 || fbId(tmp) == 0)
        return;
    if (!st.waveFb[st.waveCurrent] || !st.waveFb[st.waveCurrent]->getTexture())
        return;

    GLint prevFbo = 0, prevVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    // The compositor clips everything to the frame's DAMAGE region with a
    // scissor rect, and glClear honors the scissor. For every other offscreen
    // pass that was invisible: they overwrite their whole target each run, so
    // a clipped write healed on the next frame. The splat is ADDITIVE - a
    // clipped clear leaves last frame's light in place and the pass then
    // deposits another full helping of energy on top, every frame, until the
    // texture saturates white everywhere the scissor did not reach. That was
    // the unfocused-window strobe: the scissor tracks damage, damage tracks
    // the focused window, and every window sampling the unswept part of the
    // caustic texture lit up. Offscreen simulation passes must never be
    // scissored by screen damage.
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);

    // Piggybacked here rather than on the sim step: it needs exactly the same
    // scissor-off / blend-off / restore-the-FBO envelope, and the warp reads a
    // heavily low-passed field so this pass's ~72 Hz cadence is far more than
    // it can resolve.
    buildSmoothWave();

    const float depth = cfg.shimmerDepth ? static_cast<float>(**cfg.shimmerDepth) : 1.0f;
    constexpr float WATER_N = 1.333f;
    constexpr float SNELL   = 1.0f - 1.0f / WATER_N;
    const float lensK = std::max(depth, 0.10f) * SNELL * 0.080f;

    {
        const auto& u = sm.causticUniforms;
        auto sh = g_pHyprOpenGL->useShader(sm.causticShader);
        // Units 8-10, deliberately far from home. This pass runs mid-frame
        // between the compositor's own draws, and the compositor CACHES what
        // it believes is bound on the low units - binding the wave texture on
        // unit 0 here silently poisoned that cache, and whichever element
        // drew next without a rebind composited the WATER STATE as its
        // surface: the frozen bright flash on unfocused windows. Every other
        // sim pass already lives on units 2-6; nothing in the compositor
        // touches 8+.
        sh->setUniformInt(SHADER_TEX, 8);
        glUniform1i(u.trailTex, 9);
        glUniform1i(u.velTexG, 10);
        glUniform1f(u.waveSubFrac, st.waveSubFrac);
        glUniform1f(u.waveBias, st.waveBias);
        auto& vfb = st.fluidVelFb[st.fluidVelCurrent];
        const bool velOk = st.flowDt > 0.0f && vfb && vfb->getTexture();
        glUniform1f(u.flowShift, velOk ? st.flowDt : 0.0f);
        // A quarter of the analytic coefficient. The pointwise 1/|det| could
        // only ever draw the det=0 contour, so it tolerated a lens deep past
        // the fold; transporting the energy for real at that strength piles
        // whole regions into blown-out pools. Real pool floors live where
        // folds are just forming -- delicate vein networks -- and with h in
        // sim units rather than metres, matching that REGIME is the honest
        // calibration available.
        glUniform1f(u.causticK, lensK * 0.100f);
        glUniform1f(u.gridN, float(CAUSTIC_RES));

        glActiveTexture(GL_TEXTURE8);
        st.waveFb[st.waveCurrent]->getTexture()->bind();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (st.trailFb && st.trailFb->getTexture()) {
            glActiveTexture(GL_TEXTURE9);
            st.trailFb->getTexture()->bind();
        }
        if (velOk) {
            glActiveTexture(GL_TEXTURE10);
            vfb->getTexture()->bind();
        }
        glActiveTexture(GL_TEXTURE0);

        g_pHyprOpenGL->setViewport(0, 0, CAUSTIC_RES, CAUSTIC_RES);
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(fb));

        // Every frame starts dark and the points repaint the light. Alpha is
        // cleared to 1 and the points leave it alone. The clear color is
        // process-global GL state -- put it back the way it was found.
        GLfloat prevClear[4] = {0, 0, 0, 0};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Additive: arrivals of light sum. The compositor's cached GL state
        // tracks blend on/off but not the blend FUNC, so save and restore
        // that by hand.
        GLint srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
        glBlendFunc(GL_ONE, GL_ONE);

        // No vertex attributes anywhere in the program -- gl_VertexID is the
        // whole input -- so the default VAO is the right one to have bound.
        glBindVertexArray(0);
        glDrawArrays(GL_POINTS, 0, CAUSTIC_RES * CAUSTIC_RES);

        g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
        glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
        glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    }

    // FINITE SUN: blur radius grows with depth, which is also the relationship
    // real water has and this model used to have backwards - a point sun made
    // caustics SHARPER the deeper the water, holding the astigmatic pair apart
    // instead of merging it.
    const float scaleUp = float(CAUSTIC_RES) / float(SIM);
    // ...AND THE RECONSTRUCTION FILTER FOR THE SPLAT, which is what the floor
    // is really about. The splat is a POINT SAMPLING of a continuous density on
    // a unit lattice, so the deposited field carries an alias at the lattice
    // frequency. It is not white noise: the landing spacing drifts smoothly
    // (measured p50 1.00, p90 1.46 target texels), so the alias beats against
    // the lattice and prints a coherent dot-and-stripe weave across every dim
    // region — visible as stepped contour stripes with dotty edges, worst
    // exactly where the field is smoothest and there is nothing else to hide
    // it. Widening the deposit kernel and blurring afterwards are the SAME
    // convolution, so this is the cheap end of that identity.
    //
    // Measured on a live-settings field: the weave is gone at sigma 0.7 target
    // texels and the veins are still crisp; sigma 1.1+ starts costing real
    // sharpness. The old floor was radius 0.6, i.e. sigma 0.2 -- effectively no
    // filter at all, which is why the weave was there at every setting. Radius
    // is 3*sigma in the blur shader, so 2.1 is that measured optimum, and the
    // depth term is scaled so it passes through the same point rather than
    // sitting two decades below the floor and never doing anything.
    const float blurTexels = std::clamp(lensK * 30.0f * scaleUp, 2.1f, 12.0f);
    {
        const auto& bu = sm.blurUniforms;
        auto sh = g_pHyprOpenGL->useShader(sm.blurShader);
        sh->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        // Unit 8 for the blur input as well - same cache-poisoning rule.
        sh->setUniformInt(SHADER_TEX, 8);
        glBindVertexArray(sh->getUniformLocation(SHADER_SHADER_VAO));
        glUniform1f(bu.radius, blurTexels);
        glActiveTexture(GL_TEXTURE8);

        glBindFramebuffer(GL_FRAMEBUFFER, fbId(tmp));
        fb->getTexture()->bind();
        glUniform2f(bu.direction, 1.0f / CAUSTIC_RES, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, fbId(fb));
        tmp->getTexture()->bind();
        glUniform2f(bu.direction, 0.0f, 1.0f / CAUSTIC_RES);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (dbgLog()) {
        // What is actually IN the caustic texture after clear+splat+blur:
        // mean-1 illumination, or accumulated blow-out?
        float px[4] = {0, 0, 0, 0}; float acc[3] = {0, 0, 0};
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(fb));
        const int probes[3][2] = {{CAUSTIC_RES/4, CAUSTIC_RES/4}, {CAUSTIC_RES/2, CAUSTIC_RES/2}, {(3*CAUSTIC_RES)/4, (3*CAUSTIC_RES)/4}};
        for (int i = 0; i < 3; i++) {
            glReadPixels(probes[i][0], probes[i][1], 1, 1, GL_RGBA, GL_FLOAT, px);
            acc[i] = px[1];
        }
        DBG("%.4f CAUSVAL g=%.3f,%.3f,%.3f\n", dbgNow(), static_cast<double>(acc[0]), static_cast<double>(acc[1]), static_cast<double>(acc[2]));
    }
    g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
    if (prevScissor)
        glEnable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);

    // MIP CHAIN FOR THE CAUSTIC — this is the flickering rim on glass windows.
    //
    // The glass samples this texture at the REFRACTED coordinate, and the dome
    // refraction is steepest in the outermost columns: there the sample
    // coordinate walks many texels per screen pixel, so a single GL_LINEAR tap
    // lands on an essentially arbitrary filament of a high-frequency 1024²
    // field — and on a DIFFERENT one every time the water moves. That is
    // minification aliasing, and its signature is exactly temporal: the rim
    // flickers while the interior, where the coordinate advances about one
    // texel per pixel, sits perfectly still. Measured on an unfocused window's
    // outermost column: 0.0 swing with shimmer off, 25.3 with it on, and 0.0
    // again with shimmer on but its intensity at zero — the caustic term alone.
    //
    // A mip chain is the fix the sampler already knows how to apply. The
    // hardware measures the coordinate's own derivative per pixel and reads a
    // PRE-AVERAGED level wherever the sample is minified, which is the average
    // that column was always meant to be showing. The interior still selects
    // mip 0, so nothing there changes.
    //
    // Generated only after the FBO is unbound: this texture is the pass's own
    // colour attachment, and writing its mips while it is still attached is a
    // feedback loop.
    if (fb->getTexture()) {
        glActiveTexture(GL_TEXTURE7);
        fb->getTexture()->bind();
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    g_pHyprOpenGL->setViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
}

void stepWaveSim() {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized())
        return;

    // Crude integrator of recently-injected wave energy, for the
    // self-limiting agitation: real water cannot stack waves without bound —
    // past a point they break and the energy leaves the wave field. A height
    // field has no breaking, so this is its diminishing-returns stand-in.
    static float seaEnergy = 0.0f;
    int SUB = 1;   // sub-steps per nominal 1/120 s step (see the time block)

    // Called from the glass pass, which runs once PER GLASSED SURFACE — with
    // several glass windows up that would advance the simulation several times
    // per frame, so the water would speed up as you opened windows. Rate-limit
    // to one step per ~8ms so the surface evolves at the same pace regardless
    // of how much glass is on screen.
    // FIXED TIMESTEP, not a "has 8ms passed yet" gate.
    //
    // The old gate compared against a fixed 8ms while frames arrive every ~6ms
    // at 165Hz, so it ran on every other frame — and because frame timing
    // jitters, sometimes two frames in a row were skipped and sometimes two ran
    // back to back. The water genuinely sped up and slowed down: the "pulsing in
    // and out of full framerate", with no GPU load behind it.
    //
    // Instead accumulate real elapsed time and run however many WHOLE steps it
    // buys, so the simulation advances at a constant rate no matter how the
    // frames land — which also makes wave speed independent of refresh rate.
    int steps = 0;
    {
        using namespace std::chrono;
        // ANCHORED TO ABSOLUTE TIME, not to "elapsed since I was last called".
        //
        // Monitors at different refresh rates render at different moments, so a
        // window spanning two screens has its halves drawn at different times.
        // With an elapsed-time accumulator each half could catch a different
        // simulation state and the seam showed it as flicker. Deriving the state
        // from a clock instead means any surface, on any monitor, at any moment,
        // asks for the same answer.
        static steady_clock::time_point origin{};
        static double simTime  = 0.0;   // simulated seconds consumed so far
        static double simDone  = 0.0;   // simulated seconds already integrated
        static double lastReal = 0.0;
        constexpr double STEP  = 1.0 / 120.0;

        const auto now = steady_clock::now();
        if (origin.time_since_epoch().count() == 0) origin = now;
        const double real = duration<double>(now - origin).count();

        const auto& spcfg = g_pGlobalState->config;
        const double speed = spcfg.shimmerSpeed
            ? std::clamp(static_cast<double>(**spcfg.shimmerSpeed), 0.0, 4.0) : 1.0;

        double dReal = real - lastReal;
        if (dReal < 0.0)  dReal = 0.0;
        if (dReal > 0.25) dReal = 0.25;   // swallow stalls rather than replaying them
        lastReal = real;
        simTime += dReal * speed;
        // Injected-energy estimate for the self-limiting agitation below:
        // bleeds away on a ~2 s time constant of REAL time.
        if (g_freezeAt <= 0)
            seaEnergy *= static_cast<float>(std::exp(-dReal * 0.5));

        // SUB-STEPPING. At low speed, whole 1/120 s steps arrive so rarely
        // that the surface — and especially a drag's deposits — visibly tick
        // between states. Instead of stepping rarely at full size, step more
        // often at reduced size: SUB sub-steps of STEP/SUB with the Courant
        // number divided by SUB² keeps the physical wave speed identical
        // (c ∝ sqrt(s)·rate) while the update rate stays ≥ ~40 Hz well down
        // the slider. Never smaller than STEP/4: the Laplacian's per-step
        // contribution must stay above fp16 precision — the old "just scale
        // dt" approach died exactly there.
        SUB = speed >= 0.4 ? 1 : (speed >= 0.15 ? 2 : 4);
        const double sub = STEP / SUB;

        double pending = simTime - simDone;
        int n = static_cast<int>(pending / sub);
        const int cap = 4 * SUB;
        if (n > cap) { n = cap; simDone = simTime - cap * sub; }   // caught up after a stall
        steps = n;

        // DETERMINISTIC A/B HARNESS. HYPRWATER_FREEZE_AT=N runs exactly N
        // steps from the cleared field and then stops the world: the impulse
        // generator is keyed on waveStepCount, so two BUILDS given the same N
        // produce bit-identical water and any pixel difference is purely the
        // code change. Without it the water differs run to run and nothing
        // below ~10% is measurable — which is how several "no effect" results
        // ended up unfalsifiable.
        if (g_freezeAt > 0) {
            const long done = static_cast<long>(g_pGlobalState->waveStepCount);
            steps = done >= g_freezeAt ? 0
                  : static_cast<int>(std::min<long>(steps, g_freezeAt - done));
            g_frozen = (done + steps) >= g_freezeAt;
        }
        simDone += steps * sub;
        if (g_freezeAt > 0) {
            // Same ~0.6/second bleed as the real-time form, but counted in
            // STEPS so it cannot depend on wall clock.
            seaEnergy *= std::pow(0.9958f, static_cast<float>(steps));
            g_pendingFluidTicks += steps;
        }

        // Phase within the current sub-step, also from the clock, so both
        // halves of a monitor-spanning window blend to the same point.
        g_pGlobalState->waveSubFrac = g_frozen ? 0.0f :
            static_cast<float>(std::clamp((simTime - simDone) / sub, 0.0, 1.0));

        if (steps > 0)
            DBG("%.4f STEP-BEGIN n=%d SUB=%d\n", dbgNow(), steps, SUB);
        if (steps <= 0)
            return;
    }

    auto& fbA = g_pGlobalState->waveFb[0];
    auto& fbB = g_pGlobalState->waveFb[1];
    if (!fbA) fbA = g_pHyprRenderer->createFB("hyprwater-wave-a");
    if (!fbB) fbB = g_pHyprRenderer->createFB("hyprwater-wave-b");

    // HALF FLOAT IS REQUIRED, not a nicety. Wave heights here are ~0.01, and an
    // 8-bit UNORM quantises to 1/255 ~ 0.004 — two or three levels of signal.
    // The finite-difference Hessian then reads almost pure quantisation noise,
    // which showed up as isolated white specks instead of caustic filaments.
    // Half float is strongly preferred (8-bit quantises the wave to noise), but
    // it is NOT guaranteed to be a valid render target everywhere. If the alloc
    // does not produce a usable texture, fall back rather than sailing on with
    // null framebuffers — that is what turned a driver limitation into a
    // compositor SIGSEGV during ordinary rendering.
    static uint32_t fmt = DRM_FORMAT_ABGR16161616F;
    bool fresh = false;
    // Harness reset. Frames render between plugin load and the settings being
    // applied, so those first steps run under DEFAULT config and their count
    // depends on wall clock — which is what stopped two launches matching. Wipe
    // the world once, after the settings are certainly live, and count the
    // deterministic budget from there.
    if (g_freezeAt > 0 && !g_harnessArmed) {
        static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0)
            return;
        g_harnessArmed = true;
        g_harnessClear = true;   // consumed by the `fresh` branch below
        g_pendingFluidTicks = 0;
    }
    if (fbA->m_size.x != SIM || fbA->m_size.y != SIM) { fbA->alloc(SIM, SIM, fmt); fresh = true; }
    if (fbB->m_size.x != SIM || fbB->m_size.y != SIM) { fbB->alloc(SIM, SIM, fmt); fresh = true; }

    if (fresh && fmt == DRM_FORMAT_ABGR16161616F &&
        (!fbA->getTexture() || !fbB->getTexture() || fbId(fbA) == 0 || fbId(fbB) == 0)) {
        fmt = DRM_FORMAT_ABGR8888;
        g_pGlobalState->waveBias = 0.5f;
        fbA->alloc(SIM, SIM, fmt);
        fbB->alloc(SIM, SIM, fmt);
    }

    // Whatever happened above, refuse to proceed with an unusable target.
    if (!fbA->getTexture() || !fbB->getTexture() || fbId(fbA) == 0 || fbId(fbB) == 0)
        return;

    // A flat surface encodes as 0.5 in both channels, NOT zero: the height is
    // stored biased so a UNORM target can carry negative amplitude. Clearing to
    // black would mean "h = -0.5 everywhere", i.e. a giant step the first frame
    // would violently ring.
    // Flipping the master switch should start from still water. Leaving the
    // textures alone meant re-enabling resumed a surface that had been sloshing
    // in the dark, so the effect appeared mid-storm with no way to see it begin.
    static bool wasOn = false;
    const bool  isOn  = g_pGlobalState->config.shimmerEnabled
                     && **g_pGlobalState->config.shimmerEnabled != 0;
    if (isOn && !wasOn)
        fresh = true;
    wasOn = isOn;
    if (g_harnessClear) { fresh = true; g_harnessClear = false; }

    if (fresh) {
        for (auto* fb : {&fbA, &fbB}) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbId(*fb));
            glClearColor(g_pGlobalState->waveBias, g_pGlobalState->waveBias, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        g_pGlobalState->waveStepCount = 0;
        for (auto* fb : {&g_pGlobalState->fluidVelFb[0], &g_pGlobalState->fluidVelFb[1],
                         &g_pGlobalState->fluidPrsFb[0], &g_pGlobalState->fluidPrsFb[1],
                         &g_pGlobalState->fluidDivFb}) {
            if (*fb && fbId(*fb) != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbId(*fb));
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }

    GLint prevFbo = 0, prevVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);

    // The fluid itself lives on its own real-time tick (stepFluidFrame) —
    // here the wave only needs to know whether a usable field exists to
    // advect by. flowDt is published by the fluid tick.
    const bool fluidOn = g_pGlobalState->flowDt > 0.0f
                      && g_pGlobalState->fluidVelFb[g_pGlobalState->fluidVelCurrent]
                      && g_pGlobalState->fluidVelFb[g_pGlobalState->fluidVelCurrent]->getTexture();

    const auto& u = shaderManager.waveSimUniforms;
    auto shader = g_pHyprOpenGL->useShader(shaderManager.waveSimShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 3);

    glUniform2f(u.texelSize, 1.0f / SIM, 1.0f / SIM);
    // Courant condition: (c*dt/dx)^2 must stay below 0.5 or the explicit scheme
    // diverges into NaN within a few frames. 0.22 leaves comfortable margin.
    // Close to the 0.5 stability ceiling on purpose. At 0.22 a wavefront only
    // crossed ~22 texels between impulses, so the surface stayed a field of
    // small local dents instead of spread waves that interfere — which read as
    // grain rather than caustics.
    // FIXED. Speed must not touch the physics.
    // Scaling c^2 dt^2 was wrong twice over. Linearly it scaled dt by sqrt();
    // squared it scaled dt correctly but drove the constant to ~1.8e-6 at the
    // low end, where the laplacian's contribution per step falls BELOW HALF
    // FLOAT PRECISION and vanishes into rounding — the simulation stopped
    // behaving like a wave equation at all, which is why slow water looked like
    // a different effect rather than the same one slowed down.
    // Time is scaled by running the simulation less often instead, and the gap
    // between steps is interpolated so it still looks smooth.
    // Now a plain multiplier on the stability ceiling, not an absolute Courant
    // number -- the ceiling itself moves with viscosity.
    // Shallow-water waves travel at sqrt(g*h): DEEPER WATER IS FASTER, which is
    // why swell slows and piles up as it reaches a beach. Depth already sets the
    // refraction and the focusing, so this is the third thing it should do.
    // Capped at 1 because the stability ceiling is the real limit past that
    // point -- deeper than nominal cannot actually run faster here.
    const float dep = g_pGlobalState->config.shimmerDepth
                    ? static_cast<float>(**g_pGlobalState->config.shimmerDepth) : 1.0f;
    glUniform1f(u.waveSpeed, std::min(std::sqrt(std::max(dep, 0.01f)), 1.0f));
    // Just under 1: energy bleeds away, so agitation SETTLES instead of ringing
    // forever. This is what gives the "everyone got out of the pool" pacing.
    // Closer to 1: energy survives long enough for many disturbances to be in
    // flight at once and interfere, instead of one lonely wavefront at a time.
    // Still below 1, so agitation genuinely settles when impulses pause.
    // Also fixed, for the same reason: decay is part of the physics, and
    // scaling it changed how the water behaved rather than how fast it ran.
    // Per-SUB-step: damping is a per-step retention, so K smaller steps need
    // the K-th root to bleed the same energy per simulated second.
    glUniform1f(u.damping, SUB == 1 ? 0.9994f : std::pow(0.9994f, 1.0f / SUB));
    // Fixed, not exposed. This varies the local wave speed the way an uneven
    // bottom does, which is what stops wavefronts from staying perfect arcs.
    // There was a slider for it, but "how fake would you like the floor to be"
    // is not a real choice -- every floor is uneven, and the clean version it
    // offered is a thing that does not occur.
    glUniform1f(u.bedVariation, 1.0f);


    // Occasional localized push. Deterministic LCG rather than a random device
    // so behaviour is reproducible when debugging a bad-looking frame.
    const auto& scfg = g_pGlobalState->config;
    const float agit = scfg.shimmerAgitation ? static_cast<float>(**scfg.shimmerAgitation) : 0.5f;
    const float thick = scfg.shimmerViscosity ? static_cast<float>(**scfg.shimmerViscosity) : 0.6f;
    // agitation 0..1 -> an event every 900..6 steps, GEOMETRICALLY.
    // The old mapping was linear from 90 down to 8, which had two faults. The
    // bottom of the slider still fired 120/90 = 1.3 disturbances per SECOND --
    // that is not calm water, that is rain, and it is why the surface stayed
    // chaotic no matter how far the control came down. And a linear ramp on a
    // RATE spends most of its travel in the busy half: 90 to 8 is one order of
    // magnitude crammed into the last third of the slider. Rates are perceived
    // in ratios, so interpolate the logarithm and the whole range is usable.
    // At 0 this is one disturbance every 7.5 s of simulated time; damping only
    // removes about 40% of the energy over that gap, so the surface glides
    // rather than going flat.
    const float ag  = std::clamp(agit, 0.0f, 1.0f);
    uint64_t every = static_cast<uint64_t>(
        std::exp(std::log(900.0f) + (std::log(6.0f) - std::log(900.0f)) * ag) + 0.5f);
    // `every` counts STEPS; sub-steps are SUB× more frequent, so scale it to
    // keep the disturbance rate fixed in real time.
    every *= static_cast<uint64_t>(SUB);

    // "Ripples vs swell" IS viscosity: impulse size only chooses what goes IN,
    // viscosity chooses what SURVIVES, and the latter is what the label means.
    //
    // STABILITY. The usual Courant limit s < 0.5 does NOT survive the viscous
    // term. Writing one mode as h+ = d[(2 + (s+v)L)h - (1 + vL)h-] with L the
    // Laplacian eigenvalue on [-8,0], bounded roots need A^2 < 4B, and the
    // (1 + vL) term is what viscosity attacks: it drives B toward zero, the
    // roots go real, and one of them leaves the unit circle. Working it out at
    // L = -8 gives
    //
    //     s + v < sqrt(s / 2)
    //
    // which collapses to s < 0.5 only when v = 0. Assuming the two simply
    // shared a 0.5 budget was wrong and diverged at every setting except the
    // thinnest -- the Nyquist mode amplifying every step, which is the
    // pixel-checkerboard that ate the window.
    //
    // Solving s + v = K*sqrt(s/2) for s, in u = sqrt(s):
    //     u^2 - (K/sqrt2) u + v = 0
    // and taking the larger root gives the fastest speed this viscosity allows.
    // K is the fraction of the boundary used; 0.78 keeps the worst-case
    // per-step gain at 0.9996 instead of sitting on 1.0.
    // Reads in the same direction as its name: right is thicker.
    const float visc = 0.005f + 0.060f * std::clamp(thick, 0.0f, 1.0f);
    constexpr float K = 0.78f;
    const float disc = std::max(K * K * 0.5f - 4.0f * visc, 0.0f);
    const float uRoot = (K / std::sqrt(2.0f) + std::sqrt(disc)) * 0.5f;
    glUniform1f(u.hBias, g_pGlobalState->waveBias);
    // Courant number and viscosity both scale with dt² ∝ 1/SUB², which is
    // exactly what keeps the physical wave speed and dissipation identical
    // across sub-step regimes (and strictly inside the stability ceiling,
    // which was solved for the full-size step).
    glUniform1f(u.viscosity, visc / (SUB * SUB));
    glUniform1f(u.maxSpeed, uRoot * uRoot / (SUB * SUB));
    // Exactly 0 when currents are off: the shader's advection branch keys on
    // it, so a disabled field costs one compare and no texture read.
    glUniform1i(u.velTex, 4);
    glUniform1f(u.flowAdvect, fluidOn ? 1.0f / (120.0f * SUB) : 0.0f);
    // DELIBERATELY NOT scaled by speed. Tying it to simulated time was correct
    // in physics and wrong in use: at speed 0.002 it worked out to one
    // disturbance every ~6 MINUTES, so the surface just sat there. "Slow" is
    // wanted as slow MOTION, not as a slow world where nothing happens.
    // Keeping the rate in real time means low speed reads as a gently evolving
    // texture that is still fed, and "How often" stays an independent control.


    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);

    for (int i = 0; i < steps; i++) {
        auto& s0 = g_pGlobalState->waveFb[g_pGlobalState->waveCurrent];
        auto& d0 = g_pGlobalState->waveFb[1 - g_pGlobalState->waveCurrent];
        if (!s0 || !s0->getTexture() || fbId(d0) == 0)
            break;

        // The fluid no longer advances here — it integrates continuously on
        // its own real-time tick (stepFluidFrame), so this step simply reads
        // whatever the field currently is. The height dipole below stays even
        // with currents on, because a divergence-free field advecting a FLAT
        // surface leaves it flat — momentum alone cannot raise water here, so
        // without the dipole a drag across calm water would be invisible.
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        g_pHyprOpenGL->setViewport(0, 0, SIM, SIM);

        // Impulses are decided per STEP, so their rate follows simulated time
        // rather than however many frames happened to get drawn.
        const uint64_t nn = g_pGlobalState->waveStepCount++;
        // Zero means zero. Any nonzero rate, however slow, still eventually
        // fires, so the bottom of the slider has to be an actual OFF rather
        // than the slowest available drip -- otherwise there is no way to watch
        // the water finish settling, which is the only way to judge damping.
        // In-domain volume of one DoG deposit, in mean-height units: erf over
        // the [0,1]^2 the shader can actually write, for each of the two
        // Gaussians. Unclipped the pair cancels exactly, so only events
        // hanging over the wall produce any correction. Goes to the shader's
        // volComp uniform, which spreads it evenly across the whole sheet.
        auto gaussFrac = [](float c, float r) {
            return 0.5f * (std::erf((1.0f - c) / r) + std::erf(c / r));
        };
        auto dogVolume = [&](float x, float y, float r, float amp) {
            const float f1 = gaussFrac(x, r) * gaussFrac(y, r);
            const float f2 = gaussFrac(x, 2.0f * r) * gaussFrac(y, 2.0f * r);
            return amp * 3.14159265f * r * r * (f1 - f2);
        };
        float volComp = 0.0f;

        // ── SLOT 2: round events (ambient swell / click tap) ─────────────
        // Independent of the stroke slot below, so splashes never starve the
        // drag-trail absorption and vice versa.
        {
            auto& st2 = *g_pGlobalState;
            const bool fire = ag > 0.001f && nn % std::max<uint64_t>(every, 2) == 0;
            if (fire) {
                uint64_t r = nn * 6364136223846793005ULL + 1442695040888963407ULL;
                auto fr = [&](int sh) { return static_cast<float>((r >> sh) & 0xFFFF) / 65535.0f; };
                // Outer ring only: the shader samples just the middle of the
                // sim, so these are genuinely off-screen and travel inward.
                const float ang = fr(16) * 6.2831853f;
                // Ring moved in from 0.40-0.49 to make room for the absorbing band added in
                // wavesim.frag. At 0.40 the ring's outer edge sat at uv 0.01 - one
                // percent from a reflecting wall - so half of every injected wave
                // bounced straight back and stood against the inbound half.
                // Waves still cross ~0.24 uv before reaching the visible radius (0.105).
                const float rr  = 0.29f + 0.09f * fr(32);
                const float rad = 0.025f + 0.050f * std::clamp(thick, 0.0f, 1.0f) + 0.020f * fr(40);
                // Calm water is disturbed more GENTLY, not merely less often;
                // and SELF-LIMITING by the energy already in flight — real
                // waves break instead of stacking without bound.
                const float tame = 1.0f / (1.0f + seaEnergy * 0.12f);
                const float amp = (0.10f + 0.16f * fr(48)) * (0.45f + 0.55f * ag) * tame;
                seaEnergy += amp;
                // A SWELL, not a pop: the amplitude enters over 6 steps. At
                // low sim speed a one-step splash materialised in ~60 ms while
                // everything else crawled — the user's video caught them as
                // isolated ticks on otherwise still water.
                st2.ambX = 0.5f + std::cos(ang) * rr;
                st2.ambY = 0.5f + std::sin(ang) * rr;
                st2.ambR = rad;
                st2.ambChunk = amp / 6.0f;
                st2.ambLeft  = amp;
            }
            if (st2.click.amount > 1e-5f) {
                // A tap presses IN — negative amplitude — and the rebound ring
                // is the ripple. Taps are real-time user actions: immediate.
                const float cr = st2.click.r > 0.0f ? st2.click.r : 0.016f;
                glUniform4f(u.impulse2, st2.click.x, st2.click.y, cr, -st2.click.amount);
                volComp += dogVolume(st2.click.x, st2.click.y, cr, -st2.click.amount);
                st2.click.amount = 0.0f;
            } else if (st2.ambLeft > 0.0f) {
                const float chunk = std::min(st2.ambChunk, st2.ambLeft);
                glUniform4f(u.impulse2, st2.ambX, st2.ambY, st2.ambR, chunk);
                volComp += dogVolume(st2.ambX, st2.ambY, st2.ambR, chunk);
                st2.ambLeft -= chunk;
            } else {
                glUniform4f(u.impulse2, 0.0f, 0.0f, 1.0f, 0.0f);
            }
        }

        // ── STROKE SLOTS: drain the ring HARD, up to 8 segments per step ──
        // Segments waiting around analytically while their absorbed siblings
        // flow with the currents was the static-vs-flowing handoff chop; with
        // 8 slots the ring drains faster than any whip can fill it, so a
        // segment spends at most a step or two analytic — while the currents
        // near a fresh drag are still too weak to expose the handoff.
        {
            auto& st2 = *g_pGlobalState;
            st2.lastAbsCount = 0;
            if (st2.drag.amount > 1e-5f) {
                pushPendStroke(st2.drag);
                st2.drag.px = st2.drag.x;
                st2.drag.py = st2.drag.y;
                st2.drag.amount = 0.0f;
            }
            float seg[32] = {0}, par[32] = {0};
            int   ns = 0;
            auto slot = [&](const SGlobalState::SDrag& s, float fallbackR, bool round) {
                if (ns >= 8 || s.amount <= 1e-5f)
                    return;
                const float ra  = s.r > 0.0f ? s.r : fallbackR;
                const float len = std::hypot(s.x - s.px, s.y - s.py);
                seg[ns * 4 + 0] = round ? s.x : s.px;
                seg[ns * 4 + 1] = round ? s.y : s.py;
                seg[ns * 4 + 2] = s.x;  seg[ns * 4 + 3] = s.y;
                par[ns * 4 + 0] = round ? 0.0f : s.dx;
                par[ns * 4 + 1] = round ? 0.0f : s.dy;
                par[ns * 4 + 2] = ra;
                par[ns * 4 + 3] = s.amount / (round ? 1.0f : (1.0f + 0.6f * len / ra));
                if (round)
                    volComp += dogVolume(seg[ns * 4 + 0], seg[ns * 4 + 1], ra, par[ns * 4 + 3]);
                ns++;
            };
            while (st2.pendLen > 0 && ns < 8) {
                const auto& sgm = st2.pendRing[st2.pendHead];
                // Remember what is being absorbed: the trail pass keeps
                // rendering it this interval at (1 - subFrac), the exact
                // complement of its texture copy fading in.
                if (st2.lastAbsCount < 8)
                    st2.lastAbs[st2.lastAbsCount++] = sgm;
                slot(sgm, 0.045f, false);
                st2.pendHead = (st2.pendHead + 1) % SGlobalState::PEND_RING;
                st2.pendLen--;
            }
            glUniform4fv(u.sSeg, 8, seg);
            glUniform4fv(u.sPar, 8, par);
        }
        glUniform1f(u.volComp, volComp);

        glBindFramebuffer(GL_FRAMEBUFFER, fbId(d0));
        // Unit 3, NOT unit 0: unit 0 is the glass shader's backdrop sampler, and
        // leaving the sim texture bound there makes the glass sample the water
        // height as if it were the wallpaper (observed: windows went flat grey).
        glActiveTexture(GL_TEXTURE3);
        s0->getTexture()->bind();
        // The wave texture is read at roughly one texel per five screen
        // pixels, so its filtering is not a detail: on NEAREST the height
        // field is piecewise constant and its finite-difference curvature is
        // either zero or enormous exactly on texel boundaries, which prints
        // the simulation grid onto the window as blocky steps. Nothing ever
        // set these, so they were whatever the framebuffer allocation left
        // behind. CLAMP matters as much: the reflecting boundary needs
        // samples past the edge to repeat the edge, not wrap to the far side.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // The freshly projected velocity, for the advection lookup. Unit 4 —
        // unit 0 is the glass backdrop, unit 3 is the height field.
        if (fluidOn) {
            bindSimTexture(4, g_pGlobalState->fluidVelFb[g_pGlobalState->fluidVelCurrent]);
            glActiveTexture(GL_TEXTURE3);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        g_pGlobalState->waveCurrent = 1 - g_pGlobalState->waveCurrent;
    }

    DBG("%.4f STEP-DRAWS-DONE\n", dbgNow());
    g_pHyprOpenGL->setCapStatus(GL_BLEND, true);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);   // leave the active unit as the caller expects

    // Restore the caller's target and viewport. blurBackground() does the same;
    // leaving the sim's 512x512 viewport bound would scissor the glass pass down
    // to a corner of the screen.
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    g_pHyprOpenGL->setViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);

}

void updateAdaptiveLuma(SP<Render::IFramebuffer>& sampleFramebuffer,
                        SP<Render::IFramebuffer> lumaFb[2], int& current, bool& seeded,
                        GLuint callerFramebufferID, int viewportWidth, int viewportHeight) {
    auto& sm = g_pGlobalState->shaderManager;
    if (!sampleFramebuffer || !sm.isInitialized())
        return;

    for (int i = 0; i < 2; i++) {
        if (!lumaFb[i])
            lumaFb[i] = g_pHyprRenderer->createFB("hyprwater-adaptive-luma");
        if (lumaFb[i]->m_size.x != 1 || lumaFb[i]->m_size.y != 1)
            lumaFb[i]->alloc(1, 1, DRM_FORMAT_ABGR16161616F);
        if (!lumaFb[i]->getTexture() || fbId(lumaFb[i]) == 0)
            return;
    }

    const int prev = current;
    const int next = 1 - current;

    // Frame delta -> EMA weight, so the settle time is the same whatever the
    // compositor is running at. Clamped: a long stall must not snap the value.
    //
    // PER FRAMEBUFFER, not one static clock. A single shared timestamp gets
    // consumed by whichever glass surface renders first; every other surface in
    // the same frame then sees dt~0 and its average stops advancing. Which
    // surface wins depends on render order, and focusing a window changes that
    // order — so the smoothing rate silently depended on which window you last
    // clicked.
    static std::unordered_map<const void*, std::chrono::steady_clock::time_point> lastSeen;
    const auto  key = static_cast<const void*>(lumaFb[0].get());
    const auto  now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (auto it = lastSeen.find(key); it != lastSeen.end())
        dt = std::clamp(std::chrono::duration<float>(now - it->second).count(), 0.0f, 0.25f);
    lastSeen[key] = now;

    const auto& cfg = g_pGlobalState->config;
    const float tau = cfg.adaptiveSpeed ? std::max(0.01f, static_cast<float>(**cfg.adaptiveSpeed)) : 2.0f;
    const float alpha = 1.0f - std::exp(-dt / tau);

    // Offscreen pass: the render pass's damage scissor is in monitor coords and
    // would discard a 1x1 draw entirely. Same trap as blurBackground().
    const GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

    auto shader = g_pHyprOpenGL->useShader(sm.adaptiveLumaShader);
    static constexpr std::array<float, 9> PROJ = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, PROJ);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1i(sm.adaptiveLumaUniforms.prevTex, 1);
    glUniform1f(sm.adaptiveLumaUniforms.emaAlpha, alpha);
    glUniform1f(sm.adaptiveLumaUniforms.seedPrev, seeded ? 1.0f : 0.0f);

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(lumaFb[next]));
    g_pHyprOpenGL->setViewport(0, 0, 1, 1);

    glActiveTexture(GL_TEXTURE0);
    sampleFramebuffer->getTexture()->bind();
    // Build the mip chain so the shader can read the whole-window average from
    // the 1x1 level. Needs a mipmap min-filter to be sampled; put it back to
    // LINEAR afterwards because applyGlassEffect samples this same texture and
    // expects no mip selection.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE1);
    lumaFb[prev]->getTexture()->bind();

    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glActiveTexture(GL_TEXTURE0);
    sampleFramebuffer->getTexture()->bind();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, callerFramebufferID);
    g_pHyprOpenGL->setViewport(0, 0, viewportWidth, viewportHeight);
    if (hadScissor)
        glEnable(GL_SCISSOR_TEST);

    current = next;
    seeded  = true;
}

void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer>& tempFramebuffer,
                    float radius, int iterations,
                    GLuint callerFramebufferID, int viewportWidth, int viewportHeight) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!sampleFramebuffer || radius <= 0.0f || iterations <= 0 || !shaderManager.isInitialized())
        return;

    int width  = static_cast<int>(sampleFramebuffer->m_size.x);
    int height = static_cast<int>(sampleFramebuffer->m_size.y);

    // The caller's own scratch, sized to the caller's own sample. When this was
    // one global buffer it was resized by whichever window blurred last, so a
    // desktop of differently-sized windows reallocated it every window every
    // frame and the blur only ever came out right for one of them.
    auto& blurTempFramebuffer = tempFramebuffer;
    if (!blurTempFramebuffer)
        blurTempFramebuffer = g_pHyprRenderer->createFB("hyprwater-blur-temp");

    if (blurTempFramebuffer->m_size.x != width || blurTempFramebuffer->m_size.y != height)
        blurTempFramebuffer->alloc(width, height, sampleFramebuffer->m_drmFormat);
    // Matches whatever the sample FB ended up as, so the ping-pong never
    // narrows precision on one leg.

    // Fullscreen quad projection: maps VAO positions [0,1] to clip space [-1,1]
    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    const auto& blurUniforms = shaderManager.blurUniforms;

    auto shader = g_pHyprOpenGL->useShader(shaderManager.blurShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1f(blurUniforms.radius, radius);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);

    if (dbgLog()) {
        // Is a leftover scissor from the render pass (or a previous window's
        // composite) clipping these fullscreen quads inside the small sample FB?
        GLint sb[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_SCISSOR_BOX, sb);
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        DBG("%.4f BLURFB sample=%d(%.0fx%.0f) temp=%d rad=%.2f it=%d scissor=%d[%d,%d %dx%d] vp=[%d,%d %dx%d]\n",
            dbgNow(), fbId(sampleFramebuffer), sampleFramebuffer->m_size.x, sampleFramebuffer->m_size.y,
            fbId(blurTempFramebuffer), radius, iterations,
            glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0, sb[0], sb[1], sb[2], sb[3],
            vp[0], vp[1], vp[2], vp[3]);
    }

    // The render pass leaves a damage-rect scissor enabled, in MONITOR
    // coordinates. The quads below are fullscreen draws into a sample FB that is
    // a few hundred pixels across, so a rect like [1246,1073 925x16] doesn't
    // clip the blur — it lands entirely outside the target and throws the whole
    // pass away. The sample then composites exactly as sampled: unblurred.
    //
    // Only the first glassed window on a monitor happens to run with the scissor
    // off, so precisely one window per monitor got a blur. Focusing a floating
    // window raises it, which renders it later, which is why it was never the
    // focused one and why the blur hopped as you clicked between windows.
    //
    // sampleBackground(), the wave sim and applyGlassEffect all already guard
    // this. blurBackground was the one offscreen pass that never did.
    const GLboolean blurScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

    // Ping-pong at full resolution: sampleFramebuffer ↔ blurTempFramebuffer
    for (int iteration = 0; iteration < iterations; iteration++) {
        // Horizontal pass: sampleFramebuffer → blurTempFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(blurTempFramebuffer));
        sampleFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 1.0f / width, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Vertical pass: blurTempFramebuffer → sampleFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        blurTempFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 0.0f, 1.0f / height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (blurScissor)
        glEnable(GL_SCISSOR_TEST);

    // Restore caller's GL state without querying (avoids pipeline stalls)
    glBindFramebuffer(GL_FRAMEBUFFER, callerFramebufferID);
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, viewportWidth, viewportHeight);
}

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, float cornerRadius, float roundingPower,
                       const Vector2D& paddingRatio, const SResolveContext& resolveContext,
                       const SMaskInfo* mask, SP<Render::IFramebuffer> adaptiveLumaFb) {
    // ONE scissor guard for the ENTIRE effect, raw GL, not the compositor's
    // cached cap wrapper (the cache can disagree with real state and skip the
    // disable). The render pass leaves a damage-rect scissor enabled; every
    // offscreen op below - the backdrop sample blit, the blur quads, the sim,
    // trail, fluid and caustic passes - writes its whole target and assumes
    // it CAN. Scissored, each leaves unrefreshed texels that fossilize
    // whatever VRAM held before; the glass then re-samples its own stale
    // output and the fossil self-propagates through the swapchain forever.
    // Guarding passes one at a time just moved the fossil - this covers all
    // of them, and the final composite draw is clipped by the caller's
    // damage region geometry, not this scissor.
    const GLboolean fxScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    struct SScissorRestore {
        GLboolean was;
        ~SScissorRestore() { if (was) glEnable(GL_SCISSOR_TEST); }
    } fxScissorRestore{fxScissor};

    if (!sampleFramebuffer || !targetFramebuffer)
        return;

    auto& shaderManager  = g_pGlobalState->shaderManager;
    const auto& uniforms = shaderManager.glassUniforms;

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));

    Mat3x3 glMatrix = g_pHyprRenderer->projectBoxToTarget(rawBox, transform);
    auto texture    = sampleFramebuffer->getTexture();

    glMatrix.transpose();

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    glActiveTexture(GL_TEXTURE0);
    texture->bind();

    // Layers only: bind the temp FBO texture (rendered surface) on texture unit 1.
    // The shader samples it to mask glass to visible content and composite surface on top.
    // Windows pass mask=nullptr so this block is skipped.
    if (mask && mask->textureId != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(mask->target, mask->textureId);
        glActiveTexture(GL_TEXTURE0);
    }

    // Advance the water BEFORE binding the glass program: stepWaveSim binds its
    // own shader/FBO/viewport, so it must not run between useShader() and the
    // glass uniform uploads.
    {
        const auto& wcfg = g_pGlobalState->config;
        const bool wOn = wcfg.shimmerEnabled && **wcfg.shimmerEnabled != 0
                      && wcfg.shimmerIntensity && **wcfg.shimmerIntensity > 0.0;
        if (wOn) {
            stepWaveSim();
            // These three run per FRAME even when the slow sim does not
            // step — that independence is the whole point: the cursor is
            // real-time input, the fluid integrates real time, and the trail
            // moves with the window.
            sampleMouseWake();
            stepFluidFrame();
            renderTrailTex();
            renderCausticTex();   // after the trail: it reads it
        }
    }

    auto shader = g_pHyprOpenGL->useShader(shaderManager.glassShader);

    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);

    const auto fullSize = Vector2D(transformedBox.width, transformedBox.height);
    shader->setUniformFloat2(SHADER_FULL_SIZE,
        static_cast<float>(fullSize.x), static_cast<float>(fullSize.y));

    // Desktop-space anchor for the water. transformedBox is monitor-local, so
    // the monitor's own position is added back to get a coordinate that means
    // the same thing on every screen -- otherwise the two monitors would each
    // start their own pool at the same origin and the sheet would tear at the
    // seam.
    Vector2D monOff{0.0, 0.0};
    Vector2D desk{1920.0, 1080.0};
    // The origin counts from the far left of ALL monitors, so the reference
    // width must too. Dividing by ONE monitor's width while measuring from the
    // desktop's left edge pushed second-monitor windows far outside the field,
    // near where waves are born, so they saw their sources from much closer.
    // Bounds accumulate as each monitor renders, so no compositor-wide list is
    // needed here. Held in global state (not a function-local static) because
    // the mouse wake in stepWaveSim maps the cursor through the same bounds.
    auto& deskMax = g_pGlobalState->deskMax;
    if (const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock()) {
        monOff = mon->m_position;
        deskMax.x = std::max(deskMax.x, mon->m_position.x + mon->m_size.x);
        deskMax.y = std::max(deskMax.y, mon->m_position.y + mon->m_size.y);
    }
    desk = deskMax;
    // rawBox, not transformedBox: rawBox is what gets projected to the target,
    // so it is the one carrying the window's real position. transformedBox is
    // relative to the sampled framebuffer, which is window-sized and therefore
    // sits at the origin -- constant no matter where the window is, which is
    // why nothing moved when the window did.
    glUniform2f(uniforms.winOrigin,
                static_cast<float>(rawBox.x + monOff.x),
                static_cast<float>(rawBox.y + monOff.y));

    // A dragged edge sweeps water aside. What it deposits is proportional to
    // the DISTANCE it covered, not to how long it spent covering it, so this
    // adds a little every frame and the simulation spends whatever has built up
    // when it next steps. Dragging 500px delivers the same displacement whether
    // it took half a second or five.
    {
        const Vector2D here{rawBox.x + monOff.x, rawBox.y + monOff.y};
        struct STrack { Vector2D prev{}; Vector2D smooth{}; };
        static std::unordered_map<uint64_t, STrack> track;
        const uint64_t id = reinterpret_cast<uintptr_t>(sampleFramebuffer.get());
        auto& tr = track[id];
        auto& prev = tr.prev;
        const Vector2D vel{here.x - prev.x, here.y - prev.y};
        prev = here;
        const double mag = std::sqrt(vel.x * vel.x + vel.y * vel.y);

        // Smoothed per-window velocity for the real-time bow wave. Rises fast
        // while dragging, decays over a few frames when the window stops, so
        // the crest melts back instead of vanishing.
        DBG("%.4f GLASS fb=%lx x=%.1f y=%.1f mag=%.2f pend=%d\n",
            dbgNow(), (unsigned long)id, here.x, here.y, mag, g_pGlobalState->pendLen);
        tr.smooth.x = tr.smooth.x * 0.75 + vel.x * 0.25;
        tr.smooth.y = tr.smooth.y * 0.75 + vel.y * 0.25;
        const double smag = std::sqrt(tr.smooth.x * tr.smooth.x + tr.smooth.y * tr.smooth.y);
        {
            const float sc2 = g_pGlobalState->config.shimmerScale
                            ? static_cast<float>(**g_pGlobalState->config.shimmerScale) : 1.0f;
            const float wPhysAmt = g_pGlobalState->config.shimmerWindowPhysics
                ? std::clamp(static_cast<float>(**g_pGlobalState->config.shimmerWindowPhysics),
                             0.0f, 1.0f) : 1.0f;
            const bool wp2 = wPhysAmt > 0.001f;
            // Strength saturates by ~12 px/frame; below ~0.5 it is off.
            // Scaled by the physics slider so the bow wave — and therefore the
            // rim glint the glass derives from its slope — fades out with it.
            const float wake = (wp2 && smag > 0.5 && smag < 400.0)
                             ? static_cast<float>(std::min(smag / 12.0, 1.0)) * wPhysAmt : 0.0f;
            const double kk = 0.85 * sc2 * 2.0 * 0.105 / std::max(desk.x, 1.0);
            const Vector2D c2{here.x + rawBox.width * 0.5, here.y + rawBox.height * 0.5};
            glUniform4f(uniforms.winWake,
                        smag > 1e-5 ? static_cast<float>(tr.smooth.x / smag) : 0.0f,
                        smag > 1e-5 ? static_cast<float>(tr.smooth.y / smag) : 0.0f,
                        wake, 0.0f);
            glUniform4f(uniforms.winRectSim,
                        static_cast<float>(0.5 + (c2.x - desk.x * 0.5) * kk),
                        static_cast<float>(0.5 + (c2.y - desk.y * 0.5) * kk),
                        static_cast<float>(std::max(rawBox.width  * 0.5 * kk, 1e-4)),
                        static_cast<float>(std::max(rawBox.height * 0.5 * kk, 1e-4)));

            // (The analytic stroke trail is no longer uploaded here — it is
            // summed into its own texture once per frame by renderTrailTex,
            // and the glass shader reads it with a single tap.)
        }
        // How hard the window grips the water. Position tracking above still
        // runs while it is at zero, so turning it up mid-drag doesn't read the
        // accumulated gap as one violent shove.
        const float wPhysAmt2 = g_pGlobalState->config.shimmerWindowPhysics
            ? std::clamp(static_cast<float>(**g_pGlobalState->config.shimmerWindowPhysics),
                         0.0f, 1.0f) : 1.0f;
        const bool wPhys = wPhysAmt2 > 0.001f;
        if (wPhys && mag > 0.3 && mag < 400.0) {
            const float sc = g_pGlobalState->config.shimmerScale
                           ? static_cast<float>(**g_pGlobalState->config.shimmerScale) : 1.0f;
            // Centre, not leading edge: the dipole already puts the crest ahead
            // and the trough behind on its own.
            const Vector2D c{here.x + rawBox.width * 0.5, here.y + rawBox.height * 0.5};
            const Vector2D g{(c.x - desk.x * 0.5) / std::max(desk.x, 1.0),
                             (c.y - desk.y * 0.5) / std::max(desk.x, 1.0)};
            const Vector2D wp{0.5 + g.x * 0.85 * sc, 0.5 + g.y * 0.85 * sc};
            auto& dg = g_pGlobalState->drag;
            const float sx = static_cast<float>(0.5 + (wp.x - 0.5) * 2.0 * 0.105);
            const float sy = static_cast<float>(0.5 + (wp.y - 0.5) * 2.0 * 0.105);
            // Fresh stroke: anchor its start one frame back, so even the very
            // first spend covers the distance moved rather than a point.
            if (dg.amount <= 1e-6f) {
                const float k = static_cast<float>(0.85 * sc * 2.0 * 0.105 / std::max(desk.x, 1.0));
                dg.px = sx - static_cast<float>(vel.x) * k;
                dg.py = sy - static_cast<float>(vel.y) * k;
            }
            dg.x  = sx;
            dg.y  = sy;
            dg.dx = static_cast<float>(vel.x / mag);
            dg.dy = static_cast<float>(vel.y / mag);
            // The disturbance is as wide as the window, because the whole
            // advancing face is what pushes the water. A fixed small radius put
            // a blob in the middle of the window instead of a front along its
            // edge, which is why it looked like it came from the wrong place.
            const double halfW = rawBox.width / std::max(desk.x, 1.0)
                               * 0.85 * sc * 2.0 * 0.105 * 0.5;
            dg.r = static_cast<float>(std::clamp(halfW * 0.9, 0.02, 0.16));
            // 0.35 is the value this shipped tuned to, so a slider at 1.0 is
            // exactly the old feel and an existing `window_physics = 1` in
            // anyone's config keeps meaning what it meant.
            const float force = 0.35f * wPhysAmt2;
            dg.amount = std::min(dg.amount + static_cast<float>(mag) * 0.00035f * force,
                                 0.05f * force);
            // Same motion, second ledger: momentum for the continuous fluid
            // tick, spent every ~28 ms instead of once per wave step.
            {
                auto& ff = g_pGlobalState->fluidForceWin;
                ff.x  = dg.x;  ff.y = dg.y;
                ff.dx = dg.dx; ff.dy = dg.dy;
                ff.r  = dg.r;
                ff.amount = std::min(ff.amount + static_cast<float>(mag) * 0.00035f * force,
                                     0.05f * force);
            }
            // Subdivide by DISTANCE, per frame: once the live segment is
            // about a radius of arc long, commit it to the ring and start a
            // new one at its end. This keeps a hard whip a fine smooth curve
            // at ANY sim speed — subdividing only at sim steps turned the
            // trail into step-rate chords whose tails teleported (the tick
            // the user kept catching at minimum speed).
            if (std::hypot(dg.x - dg.px, dg.y - dg.py) > std::max(0.9f * dg.r, 0.004f)
                && pushPendStroke(dg)) {
                dg.px = dg.x;
                dg.py = dg.y;
                dg.amount = 0.0f;
            }
        }
    }

    glUniform2f(uniforms.winSize,
                static_cast<float>(rawBox.width), static_cast<float>(rawBox.height));
    glUniform2f(uniforms.deskSize,
                static_cast<float>(desk.x), static_cast<float>(desk.y));

    glUniform1f(uniforms.refractionStrength,  resolvePresetFloat(resolveContext, &SPresetValues::refractionStrength, &SOverridableConfig::refractionStrength));
    glUniform1f(uniforms.chromaticAberration, resolvePresetFloat(resolveContext, &SPresetValues::chromaticAberration, &SOverridableConfig::chromaticAberration));
    glUniform1f(uniforms.fresnelStrength,     resolvePresetFloat(resolveContext, &SPresetValues::fresnelStrength, &SOverridableConfig::fresnelStrength));
    glUniform1f(uniforms.specularStrength,    resolvePresetFloat(resolveContext, &SPresetValues::specularStrength, &SOverridableConfig::specularStrength));
    glUniform1f(uniforms.glassOpacity,        resolvePresetFloat(resolveContext, &SPresetValues::glassOpacity, &SOverridableConfig::glassOpacity) * alpha);
    glUniform1f(uniforms.edgeThickness,       resolvePresetFloat(resolveContext, &SPresetValues::edgeThickness, &SOverridableConfig::edgeThickness));
    glUniform1f(uniforms.lensDistortion,      resolvePresetFloat(resolveContext, &SPresetValues::lensDistortion, &SOverridableConfig::lensDistortion));

    // ── Animated caustics ───────────────────────────────────────────────────
    // Global rather than per-preset on purpose: this is a "does the desktop
    // move" decision, not a per-window look, and the effects governor toggles
    // it as one switch. Intensity is forced to exactly 0 when disabled so the
    // shader's early-out skips the wave sum entirely — a disabled shimmer must
    // cost nothing, not merely a little.
    //
    // The clock is monotonic seconds, wrapped at 3600 before reaching the GPU.
    // A float32 holding uptime-in-seconds loses sub-frame resolution after a
    // few days of uptime (mantissa runs out), and the wave field would visibly
    // quantise on a machine that is never rebooted — which is this one.
    {
        const auto& cfg = g_pGlobalState->config;
        const bool  on  = cfg.shimmerEnabled && **cfg.shimmerEnabled != 0;
        const float intensity = on && cfg.shimmerIntensity ? static_cast<float>(**cfg.shimmerIntensity) : 0.0f;
        const float speed     = cfg.shimmerSpeed ? static_cast<float>(**cfg.shimmerSpeed) : 0.35f;
        const float scale     = cfg.shimmerScale ? static_cast<float>(**cfg.shimmerScale) : 1.0f;

        float t = 0.0f;
        if (intensity > 0.0f) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            t = static_cast<float>(ms % 3600000LL) / 1000.0f;
        }
        glUniform1f(uniforms.uTime,            t);
        glUniform1f(uniforms.shimmerIntensity, intensity);
        glUniform1f(uniforms.shimmerSpeed,     speed);
        glUniform1f(uniforms.shimmerScale,     scale);
        glUniform1f(uniforms.shimmerDepth,
                    cfg.shimmerDepth ? static_cast<float>(**cfg.shimmerDepth) : 1.0f);
        glUniform1f(uniforms.waveSubFrac, g_pGlobalState->waveSubFrac);
        glUniform1f(uniforms.waveTexel, 1.0f / SIM);
        glUniform1f(uniforms.waveBias, g_pGlobalState->waveBias);
        glUniform1f(uniforms.shimmerAbsorption,
                    cfg.shimmerAbsorption ? static_cast<float>(**cfg.shimmerAbsorption) : 1.0f);
        glUniform1f(uniforms.shimmerMurk,
                    cfg.shimmerMurk ? static_cast<float>(**cfg.shimmerMurk) : 0.0f);
        glUniform1i(uniforms.shimmerLightFromBackdrop,
                    (cfg.shimmerLightFromBackdrop && **cfg.shimmerLightFromBackdrop != 0) ? 1 : 0);

        // Bind the CURRENT surface. The simulation itself is stepped before the
        // glass shader is bound (see above) — stepping it here would switch the
        // active program mid-uniform-upload and every glass uniform after this
        // point would be written to the wrong program.
        if (intensity > 0.0f) {
            auto& wfb = g_pGlobalState->waveFb[g_pGlobalState->waveCurrent];
            if (wfb && wfb->getTexture()) {
                glActiveTexture(GL_TEXTURE2);
                wfb->getTexture()->bind();
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(uniforms.waveTex, 2);
            }
            // The band-limited copy the refraction warp reads — see
            // buildSmoothWave(). Unit 11: 0-7 are taken here and 8-10 belong to
            // the offscreen passes. Set unconditionally; an unbound unit samples
            // as a CONSTANT, and only this field's GRADIENT is ever used, so a
            // frame before the texture exists is calm glass, never garbage.
            glUniform1i(uniforms.waveSmoothTex, 11);
            glUniform1f(uniforms.waveSmoothTexel, 1.0f / static_cast<float>(WAVE_SMOOTH_RES));
            if (auto& swfb = g_pGlobalState->waveSmoothFb; swfb && swfb->getTexture()) {
                glActiveTexture(GL_TEXTURE11);
                swfb->getTexture()->bind();
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glActiveTexture(GL_TEXTURE0);
            }
            // The analytic stroke trail, freshly summed this frame. Unit 5 —
            // 0 backdrop, 1 layer mask, 2 water, 3/4 sim internals. The
            // uniform is set UNCONDITIONALLY: if it defaulted to unit 0 the
            // height read would add the BACKDROP's red channel to the water.
            // An unbound unit 5 samples as zero, which is merely a flat trail.
            glUniform1i(uniforms.trailTex, 5);
            auto& tfb = g_pGlobalState->trailFb;
            if (tfb && tfb->getTexture()) {
                glActiveTexture(GL_TEXTURE5);
                tfb->getTexture()->bind();
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glActiveTexture(GL_TEXTURE0);
            }
            // Velocity field for advected interpolation, unit 6. flowShift is
            // forced to 0 whenever the texture is unusable so the shader
            // falls back to the plain crossfade.
            // The precomputed caustic, unit 7.
            glUniform1i(uniforms.causticTex, 7);
            if (auto& cfb = g_pGlobalState->causticFb; cfb && cfb->getTexture()) {
                glActiveTexture(GL_TEXTURE7);
                cfb->getTexture()->bind();
                // Trilinear on minification so the mip chain built at the end
                // of renderCausticTex actually gets used — see the note there.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glActiveTexture(GL_TEXTURE0);
            }
            glUniform1i(uniforms.velTexG, 6);
            auto& vfb = g_pGlobalState->fluidVelFb[g_pGlobalState->fluidVelCurrent];
            const bool velOk = g_pGlobalState->flowDt > 0.0f && vfb && vfb->getTexture();
            glUniform1f(uniforms.flowShift, velOk ? g_pGlobalState->flowDt : 0.0f);
            if (velOk) {
                glActiveTexture(GL_TEXTURE6);
                vfb->getTexture()->bind();
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glActiveTexture(GL_TEXTURE0);
            }
        }
    }

    uploadThemeUniforms(resolveContext);

    const int64_t tintColorValue = resolvePresetInt(resolveContext, &SPresetValues::tintColor, &SOverridableConfig::tintColor);
    glUniform3f(uniforms.tintColor,
        static_cast<float>((tintColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 8) & 0xFF) / 255.0f);
    glUniform1f(uniforms.tintAlpha,
        static_cast<float>(tintColorValue & 0xFF) / 255.0f);
    {
        const auto& cfgAd = g_pGlobalState->config;
        // No 1x1 luma yet (first frame, or alloc failed) means no trustworthy
        // window average — leave the glass alone rather than guess at a dim.
        const bool haveLuma = adaptiveLumaFb && adaptiveLumaFb->getTexture();
        glUniform1f(uniforms.adaptiveTint,
            (haveLuma && cfgAd.adaptiveTint) ? static_cast<float>(**cfgAd.adaptiveTint) : 0.0f);
        glUniform1f(uniforms.adaptiveTarget,
            cfgAd.adaptiveTarget ? static_cast<float>(**cfgAd.adaptiveTarget) : 0.18f);
        glUniform1i(uniforms.adaptiveLumaTex, 4);
        if (haveLuma) {
            glActiveTexture(GL_TEXTURE4);
            adaptiveLumaFb->getTexture()->bind();
            glActiveTexture(GL_TEXTURE0);
        }
    }
    if (dbgLog()) {
        // Read BACK the effective program state right before the draw: any
        // mismatch means something rewrote a uniform after the upload above,
        // and a changing program id means the draw is not even using the
        // program the locations belong to.
        GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        GLint lfbEff = -7; GLfloat taEff = -7.0f;
        glGetUniformiv(prog, uniforms.shimmerLightFromBackdrop, &lfbEff);
        glGetUniformfv(prog, uniforms.tintAlpha, &taEff);
        const auto& vcfg = g_pGlobalState->config;
        const int   lfbWant = (vcfg.shimmerLightFromBackdrop && **vcfg.shimmerLightFromBackdrop != 0) ? 1 : 0;
        const float taWant  = static_cast<float>(tintColorValue & 0xFF) / 255.0f;
        GLfloat brEff = -7.0f, saEff = -7.0f, adEff = -7.0f, goEff = -7.0f;
        glGetUniformfv(prog, uniforms.brightness, &brEff);
        glGetUniformfv(prog, uniforms.saturation, &saEff);
        glGetUniformfv(prog, uniforms.adaptiveDim, &adEff);
        glGetUniformfv(prog, uniforms.glassOpacity, &goEff);
        GLint prevActive = 0, tex0 = -1, tex2 = -1, tex5 = -1, tex6 = -1, tex7 = -1;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2);
        glActiveTexture(GL_TEXTURE5); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex5);
        glActiveTexture(GL_TEXTURE6); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex6);
        glActiveTexture(GL_TEXTURE7); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex7);
        glActiveTexture(static_cast<GLenum>(prevActive));
        DBG("%.4f TEXBIND org=%.0f,%.0f t0=%d t2=%d t5=%d t6=%d t7=%d\n", dbgNow(),
            rawBox.x + monOff.x, rawBox.y + monOff.y, tex0, tex2, tex5, tex6, tex7);
        DBG("%.4f VERIFY org=%.0f,%.0f dark=%d pre=%s prog=%d lfb=%d/%d ta=%.3f/%.3f br=%.3f sa=%.3f ad=%.3f go=%.3f%s\n",
            dbgNow(), rawBox.x + monOff.x, rawBox.y + monOff.y,
            resolveContext.isDark ? 1 : 0, resolveContext.presetName.c_str(), prog,
            lfbEff, lfbWant, static_cast<double>(taEff), static_cast<double>(taWant),
            static_cast<double>(brEff), static_cast<double>(saEff),
            static_cast<double>(adEff), static_cast<double>(goEff),
            (lfbEff != lfbWant || std::abs(taEff - taWant) > 0.004f) ? "  MISMATCH" : "");
    }

    glUniform2f(uniforms.uvPadding,
        static_cast<float>(paddingRatio.x),
        static_cast<float>(paddingRatio.y));

    // Layers only: enable mask and provide UV mapping from the glass quad into
    // the monitor-sized temp FBO. Windows use useMask=0 (no masking).
    if (mask && mask->textureId != 0) {
        glUniform1i(uniforms.useMask, 1);
        glUniform1i(uniforms.maskTex, 1);
        glUniform2f(uniforms.maskUVOffset,
            static_cast<float>(mask->uvOffset.x),
            static_cast<float>(mask->uvOffset.y));
        glUniform2f(uniforms.maskUVScale,
            static_cast<float>(mask->uvScale.x),
            static_cast<float>(mask->uvScale.y));
        glUniform1f(uniforms.maskAlphaThreshold, mask->alphaThreshold);
    } else {
        glUniform1i(uniforms.useMask, 0);
        glUniform1f(uniforms.maskAlphaThreshold, 0.001f);
    }

    shader->setUniformFloat(SHADER_RADIUS, cornerRadius);
    shader->setUniformFloat(SHADER_ROUNDING_POWER, roundingPower);

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->scissor(rawBox);
    // PIN THE BLEND FUNCTION FOR THE COMPOSITE — this is the flickering edge
    // pixel on glassed windows.
    //
    // The shader outputs PREMULTIPLIED alpha, so the one correct func is
    // (ONE, ONE_MINUS_SRC_ALPHA). But this draw ran with whatever func was
    // ambient — and the offscreen passes above (mouse wake, caustic, trail)
    // are THROTTLED, so on frames where one of them ran, the ambient func at
    // this point differed from frames where none did. Premultiplied and
    // straight blending agree EXACTLY wherever alpha is 1 — the whole window
    // interior — and disagree only where alpha < 1: the single antialiased
    // boundary pixel. Which is precisely what was measured: a 1px edge column
    // flipping between two values while the interior stayed bit-identical,
    // rate-locked to the 14ms pass throttle (never with the water off, always
    // with the throttle at 0.6s, rock-stable with the passes forced to every
    // frame), immune to zeroing every water term, to freezing the sim, and to
    // full-monitor damage — because it was never the water's CONTENT, it was
    // the blend state its passes left behind.
    //
    // Raw GL, not the compositor's cached wrapper, for the same reason as the
    // scissor guard at the top. Saved and restored so the rest of the frame
    // sees exactly the state it would have without us.
    {
        GLint pSrcRGB = 0, pDstRGB = 0, pSrcA = 0, pDstA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB,   &pSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB,   &pDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &pSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &pDstA);
        const GLboolean pOn = glIsEnabled(GL_BLEND);
        if (dbgLog())
            DBG("BLENDSTATE on=%d src=%x dst=%x srcA=%x dstA=%x\n",
                (int)pOn, pSrcRGB, pDstRGB, pSrcA, pDstA);

        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBlendFuncSeparate(pSrcRGB, pDstRGB, pSrcA, pDstA);
        if (!pOn)
            glDisable(GL_BLEND);
    }
    if (dbgLog()) {
        // The draw just landed in fbId(targetFramebuffer). Read one pixel of
        // what was actually written: if this stays dark while the SCREEN
        // strobes bright, the wash frames never contained this draw at all
        // and the bug is in which framebuffer the pass was routed to.
        GLint curFb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFb);
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(static_cast<GLint>(targetFramebuffer->m_size.x / 2),
                     static_cast<GLint>(targetFramebuffer->m_size.y / 2),
                     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        DBG("%.4f OUTPX org=%.0f,%.0f fb=%d tgt=%d px=%d,%d,%d\n", dbgNow(),
            rawBox.x + monOff.x, rawBox.y + monOff.y,
            curFb, fbId(targetFramebuffer), px[0], px[1], px[2]);
    }
    g_pHyprOpenGL->scissor(nullptr);
}

} // namespace GlassRenderer
