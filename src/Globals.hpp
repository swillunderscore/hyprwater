#pragma once

#include "GlassLayerSurface.hpp"
#include "PluginConfig.hpp"
#include "ShaderManager.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class CGlassDecoration;

struct SGlobalState {
    std::vector<WP<CGlassDecoration>> decorations;
    CShaderManager                    shaderManager;
    SPluginConfig                     config;

    // User-defined presets (populated from config keyword, swapped in on configReloaded)
    std::unordered_map<std::string, SCustomPreset> customPresets;

    // NOTE: the blur ping-pong scratch buffer used to live here, shared by every
    // decoration "since they render sequentially". Sequentially is true and not
    // enough: the buffer is sized to the window being blurred, so with windows of
    // DIFFERENT sizes on screen it reallocates for each one, every frame, and only
    // whichever window it currently matches comes out blurred. Focusing a floating
    // window raises it, which changes render order, which changes who wins — so the
    // blur appeared to hop between windows and never landed on the focused one.
    // It is now a per-decoration member (m_blurTempFramebuffer), like m_sampleFramebuffer.

    // Wave simulation state. Two framebuffers ping-ponged one step per frame;
    // this is the whole reason the surface can be non-stationary — the field
    // has HISTORY, which no closed-form sum of oscillators has.
    SP<Render::IFramebuffer> waveFb[2];
    int      waveCurrent   = 0;
    uint64_t waveStepCount = 0;
    float    waveSubFrac   = 0.0f;   // 0..1 between the last two sim states
    // Height offset used when storing the surface. Zero on a signed float
    // target; 0.5 only on the UNORM fallback, which cannot hold a negative.
    float    waveBias      = 0.0f;
    // Impulses queued by window motion. A window is a bucket: accelerate it and
    // the water inside piles up against the trailing wall. Filled while drawing
    // each window, drained by the next simulation step.
    // Displacement waiting to be handed to the simulation. NOT a queue: the
    // render loop runs faster than the sim, so discrete events either pile up
    // or fall behind. This accumulates how far an edge has swept since the last
    // step and spends the lot in one go, which also makes the total depend on
    // DISTANCE covered rather than on how long the drag took.
    // px,py anchor the START of the stroke being accumulated (where the
    // source was when the sim last spent it); x,y are where it is NOW. The
    // wave step lays the dipole along that segment, so a slow simulation
    // getting dragged through quickly deposits a continuous ridge instead of
    // one blob per step.
    struct SDrag { float x = 0, y = 0, px = 0, py = 0, dx = 0, dy = 0, amount = 0, r = 0; };
    SDrag drag;
    // The mouse's own wake — same accumulator shape, far lighter touch. A
    // cursor is a fingertip trailed in the pool, not a hull.
    SDrag mouse;
    // Clicks are TAPS: a small round press-in at the cursor, queued by the
    // mouse-button listener and spent by the next simulation step.
    SDrag click;

    // Ring of recently finished drag strokes, still rendered analytically in
    // the glass shader while they wait to be absorbed into the simulation
    // (oldest at pendHead). Absorbing only the tail — long after the eye has
    // moved on — is what keeps a fast curved whip smooth at low sim speed.
    // Strokes are subdivided by DISTANCE as the window moves (per frame, not
    // per sim step), so a hard whip at minimum sim speed produces many fine
    // segments — the ring must hold a second or two of them. Rendering cost
    // is constant regardless: the trail is summed into its own texture once
    // per frame (trailFb), and the glass shader reads one tap.
    static constexpr int PEND_RING = 24;
    SP<Render::IFramebuffer> trailFb;
    // Caustic illumination, rendered once per frame and BLURRED (the sun is not
    // a point, so the pattern that lands on the floor is the sum over its
    // angular width). Per-screen-pixel evaluation could not afford that blur.
    SP<Render::IFramebuffer> causticFb;
    SP<Render::IFramebuffer> causticTmpFb;
    SDrag pendRing[PEND_RING];
    int   pendHead = 0;
    int   pendLen  = 0;
    // The strokes absorbed by the CURRENT sim step (up to 8 per step now):
    // rendered analytically one interval longer, weighted (1 - waveSubFrac)
    // in the trail pass so their analytic half fades out exactly as their
    // texture half fades in.
    SDrag lastAbs[8];
    int   lastAbsCount = 0;

    // Ambient splash being fed into the sim as a SWELL over several steps
    // (one-step entry read as a tick on slow water).
    float ambX = 0, ambY = 0, ambR = 1;
    float ambLeft = 0, ambChunk = 0;

    // One sim step's advection time, for the glass shader's advected
    // interpolation between states (0 while currents are off).
    float flowDt = 0.0f;

    // Momentum accumulated for the CONTINUOUS fluid tick (stepFluidFrame):
    // window and mouse motion recorded the frame it happens, spent by the
    // next ~28 ms fluid tick. The fluid used to be forced once per WAVE step
    // — a whole second of whip momentum landing as one lump at low sim speed,
    // which made the water's apparent motion surge in step-rate beats no
    // matter how smoothly the frames rendered.
    SDrag fluidForceWin;
    SDrag fluidForceMouse;

    // Logical desktop bounds, accumulated as monitors render (no compositor-
    // wide list is in scope here). Shared by the window-drag mapping and the
    // mouse-wake mapping so both live on the same sheet of water.
    Vector2D deskMax{1920.0, 1080.0};

    // Currents (Stable Fluids) state. The height field above is a SCALAR — it
    // has no curl, so it cannot hold an eddy no matter how it is forced. This
    // velocity field can. vel ping-pongs through advect/project each step; prs
    // ping-pongs inside the Jacobi solve and is kept WARM across steps so the
    // 24 iterations per step compound instead of starting over.
    SP<Render::IFramebuffer> fluidVelFb[2];
    SP<Render::IFramebuffer> fluidPrsFb[2];
    SP<Render::IFramebuffer> fluidDivFb;
    int fluidVelCurrent = 0;
    int fluidPrsCurrent = 0;

    // Layer surface glass state (one per tracked layer, keyed by raw pointer).
    // shared_ptr so CGlassLayerPassElement can hold a copy that survives map erasure mid-frame.
    std::unordered_map<Desktop::View::CLayerSurface*, std::shared_ptr<CGlassLayerSurface>> layerSurfaces;

    // Parsed namespace whitelist (empty = match all when layers enabled)
    std::unordered_set<std::string> layerNamespaceFilter;
    // Parsed namespace blacklist (always excluded, takes priority over whitelist)
    std::unordered_set<std::string> layerNamespaceExclude;
    // Per-namespace preset overrides (namespace → preset name)
    std::unordered_map<std::string, std::string> layerNamespacePresets;
    // Per-namespace mask alpha threshold (namespace → threshold, default 0.001)
    std::unordered_map<std::string, float> layerNamespaceMaskThresholds;

    // Per-monitor generation counter, incremented when the scene behind layers
    // changes on that monitor. Layer surfaces compare to their cached value to
    // skip redundant blur work. Per-monitor avoids cross-monitor feedback loops
    // where re-sampling on an idle monitor captures its own stale glass output.
    std::unordered_map<Monitor::CMonitor*, uint64_t> sceneGeneration;

    uint64_t getSceneGeneration(Monitor::CMonitor* mon) const {
        auto it = sceneGeneration.find(mon);
        return it != sceneGeneration.end() ? it->second : 0;
    }
    void bumpSceneGeneration(Monitor::CMonitor* mon) { sceneGeneration[mon]++; }

    // renderLayer hook
    CFunctionHook* renderLayerHook = nullptr;
    CFunctionHook* renderWindowHook = nullptr;
};

using Render::GL::g_pHyprOpenGL;

inline HANDLE                        PHANDLE = nullptr;
inline std::unique_ptr<SGlobalState> g_pGlobalState;

inline constexpr std::string_view PLUGIN_NAME        = "hyprwater";
inline constexpr std::string_view PLUGIN_DESCRIPTION = "Liquid glass with a simulated water surface";
// This fork's author, not upstream's. Reporting "hyprwater by Hyprnux" would
// credit them for work they did not do, and BSD-3 clause 3 specifically bars
// using their name to promote a derived product. Upstream is credited in
// LICENSE and in the README instead, which is where attribution belongs.
inline constexpr std::string_view PLUGIN_AUTHOR      = "Swill Software";
inline constexpr std::string_view PLUGIN_VERSION     = "1.0.0";
