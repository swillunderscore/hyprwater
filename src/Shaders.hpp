// Auto-generated shader header - Do not edit!
#pragma once

#include <unordered_map>
#include <string>

inline const std::unordered_map<std::string, const char*> SHADERS = {
    {"liquidglass.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Apple-style Liquid Glass Fragment Shader — Thick-glass refraction model
 *
 * The window is modeled as a thick convex glass slab:
 *   - Center: flat surface → clean frosted blur, no distortion
 *   - Edges: curved surface → refraction pulls in content from beyond
 *     the window boundary, creating natural color bleeding
 *
 * Rendering layers:
 * 1. Edge refraction via smooth outward direction + exponential proximity
 * 2. Chromatic aberration (per-channel refraction scale)
 * 3. Edge raw-texture blend for vivid color pickup
 * 4. Subtle center dome lens magnification
 * 5. Frosted tint (brightness boost + desaturation)
 * 6. Configurable color tint overlay
 * 7. Fresnel edge glow
 * 8. Specular highlight (top)
 * 9. Inner shadow (bottom rim)
 */

uniform sampler2D tex;
uniform vec2 fullSize;
uniform float radius;
uniform vec2 uvPadding;

uniform float refractionStrength;
uniform float chromaticAberration;
uniform float fresnelStrength;
uniform float specularStrength;
uniform float glassOpacity;
uniform float edgeThickness;
uniform vec3 tintColor;
uniform float tintAlpha;
uniform float lensDistortion;
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float vibrancy;
uniform float vibrancyDarkness;
uniform float adaptiveDim;
uniform float adaptiveBoost;
uniform float roundingPower;

uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;

// Animated caustics. shimmerIntensity == 0.0 skips the whole block, so the
// static path costs exactly what it did before.
uniform float uTime;
uniform float shimmerIntensity;
uniform float shimmerSpeed;
uniform float shimmerScale;
uniform int   shimmerLightFromBackdrop;
uniform sampler2D waveTex;   // R = h(t), G = h(t-1), biased +0.5
uniform float shimmerDepth;  // water depth = projection distance to the floor
uniform float waveSubFrac;   // 0..1 between the two stored sim states
uniform float waveTexel;     // 1 / simulation grid size, for the C1 read
// Real-time bow wave of THIS window while it is being dragged. In a real
// fluid the incompressible pressure response around a moving body is
// INSTANTANEOUS — only the waves it sheds propagate at wave speed — so this
// displacement rides with the window at full frame rate no matter how slowly
// the simulation itself is running. The sim keeps the persistent wake; this
// is the part that must never lag or tick.
uniform vec4 winWake;        // xy = drag direction, z = strength (0 at rest)
uniform vec4 winRectSim;     // xy = window centre, zw = half-extents (sim uv)
// The analytic stroke trail, pre-rendered once per frame into its own
// texture (trail.frag) in the same sim-uv domain as waveTex. Strokes are
// subdivided by DISTANCE as the window moves — independent of the sim step
// rate — so a hard whip at minimum speed stays a fine smooth curve instead
// of a chain of step-rate chords.
uniform sampler2D trailTex;
// Caustic illumination, precomputed and BLURRED in its own pass. 1.0 = still
// water. Evaluating it here per screen pixel could not afford the blur the
// finite size of the sun requires, and cost several times more besides.
uniform sampler2D causticTex;
// The velocity field, for ADVECTED interpolation between sim states. With
// currents on, each sim step shifts the height field along the flow; a plain
// crossfade between two SHIFTED copies dissolves instead of sliding, which
// reads as delayed, ticking motion at low sim speed — worst exactly where a
// drag just injected currents. Sampling the old state slid forward by the
// elapsed fraction of a step and the new state slid back by the remainder
// makes advected water glide continuously between steps.
uniform sampler2D velTexG;
uniform float     flowShift;   // one step's advection time; 0 = currents off
uniform float shimmerMurk;   // suspended particles: 0 = distilled, 1 = pond
uniform float shimmerAbsorption; // 1 = the measured spectrum, 0 = colorless water
// Where this window sits on the desktop, and how big the desktop is, both in
// pixels. The water used to be mapped per-window, so every window showed its
// own private pool and the same wave appeared in all of them at once. Mapping
// it in DESKTOP coordinates instead makes it one continuous sheet that the
// windows are viewports onto: a point in the water is a point on your screen.
// That is what lets a disturbance land where the cursor actually is, and it
// makes a moving window slide across standing water instead of dragging its
// own pattern along with it.
uniform vec2  winOrigin;
// Window size in the SAME logical pixels as winOrigin. fullSize comes from the
// transformed box and is a different space; adding the two put every window's
// water somewhere else depending on its size and monitor.
uniform vec2  winSize;
uniform vec2  deskSize;
uniform float waveBias;       // storage offset used by the simulation

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// ============================================================================
// TEXTURE SAMPLING (window UV -> padded texture UV)
// ============================================================================

vec2 toTexUV(vec2 wuv) {
    return wuv * (1.0 - 2.0 * uvPadding) + uvPadding;
}

vec4 sampleBlurred(vec2 wuv) {
    vec2 tuv = toTexUV(wuv);
    return texture(tex, clamp(tuv, 0.001, 0.999));
}

// ============================================================================
// SDF
// ============================================================================

float lpNorm(vec2 v, float p) {
    return pow(pow(abs(v.x), p) + pow(abs(v.y), p), 1.0 / p);
}

float getRoundedBoxSDF(vec2 uv, float r) {
    vec2 p = (uv - 0.5) * fullSize;
    vec2 halfSize = fullSize * 0.5;
    float clampedR = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + clampedR;
    return min(max(q.x, q.y), 0.0) + lpNorm(max(q, 0.0), roundingPower) - clampedR;
}

float getCornerSDF(vec2 uv) {
    return getRoundedBoxSDF(uv, radius);
}

// ============================================================================
// REFRACTION DIRECTION
// Pixel-space direction toward window center — perfectly smooth everywhere,
// no SDF gradient needed. On straight edges the perpendicular pixel distance
// dominates, giving approximately edge-normal direction. At corners it
// naturally follows the diagonal.
// ============================================================================

// ============================================================================
// WAVE-FIELD CAUSTICS
//
// This deliberately does NOT use value/Perlin noise. Procedural noise is
// stationary and band-limited: statistically identical everywhere, forever.
// The eye reads that as fake — nothing is ever surprised, and a loop point is
// always lurking. Real caustics are not a texture, they are what happens when
// light refracts through a moving surface, and real water is a superposition
// of wave trains whose periods do not divide into each other.
//
// So: sum a handful of directional waves at IRRATIONAL frequency ratios
// (1, φ, φ², … — φ being the golden ratio, the least-well-approximated-by-
// rationals number there is). The sum therefore has no common period: it never
// repeats, so there is no seam to notice. Amplitude falls roughly as 1/f,
// matching how energy distributes in real wave fields — big slow swells carry
// the motion, small fast ripples only decorate it.
//
// The caustic itself is the LAPLACIAN of that height field, not the height.
// Light focuses where the surface is concave and spreads where it is convex,
// so brightness follows curvature. That is why this reads as light through
// water rather than as a moving texture pasted on top.
// ============================================================================

const float PHI = 1.61803398875;

// Wave field height AND its exact second derivatives, from one pass.
//
// h(p) = SUM amp_i * sin(k_i . p + w_i t), so every second derivative is just
// that same sin scaled by the wave vector — no finite differences, no extra
// sin() calls. Four sample points were being spent on a numerical Laplacian
// that was both slower and blurrier than the analytic answer.
// Returns (h_xx, h_yy, h_xy).
// ============================================================================
//  SIMULATED SURFACE  (replaces every analytic wave sum that came before)
//
//  h now comes from wavesim.frag, which integrates d2h/dt2 = c^2*lap(h) on a
//  ping-ponged texture. That field has HISTORY: disturbances start somewhere,
//  spread at finite speed, reflect off the edges and interfere with their own
//  reflections, and decay. Every closed-form version tried before this was
//  statistically stationary — identical statistics everywhere, forever — which
//  is precisely the property human pattern recognition locks onto, and no
//  amount of retuning could remove it.
//
//  Derivatives are finite differences of the texture rather than analytic.
//  Noisier, but the surface is the real object now; the noise is honest.
// ============================================================================

// Only the CENTRE of the simulation is ever visible. Disturbances are injected
// in the outer ring, so waves always ARRIVE from off-screen rather than
// appearing out of nowhere in the middle of a window — the "smash" that was so
// obvious before. The ring is off-screen in every direction regardless of how
// the user's monitors are arranged, so no layout detection is needed.
// Half-width of the visible window into the sim. Smaller = the injection ring
// sits proportionally further outside what you can see, so a disturbance has
// more water to cross before it arrives. At 0.34 the ring (radius 0.40-0.49)
// landed only just past the visible edge and the "smash" was still catchable in
// the corner of your eye.
const float VIS = 0.105;

float waveH(vec2 q) {
    // NEITHER fract() NOR clamp(). fract() wrapped, putting a hard seam wherever
    // the coordinate crossed 0/1 — which the edge-refraction zone then stretched
    // into blocky banding along the border. clamp() would instead freeze the
    // value out there, i.e. a flat dead band at the edge: a different artifact,
    // not a fix.
    // The mapping is simply continuous. VIS=0.34 means q in [0,1] uses only the
    // middle third of the texture, so q can wander well outside [0,1] — which is
    // exactly what refraction does to it near an edge — and still land inside
    // real simulated water. GL_CLAMP_TO_EDGE only ever engages far outside the
    // window, where nothing is drawn.
    vec2 uv = 0.5 + (q - 0.5) * (2.0 * VIS);
    // NOTE: a quintic-ease "smooth bilinear" remap was tried here and REVERTED
    // — it flattens each texel's interior and concentrates the change at the
    // borders, which reads as a LOWER-resolution surface, not a smoother one.
    // The right tool for magnification is a genuine bicubic, if it is ever
    // worth its taps.
    // R is the newest state, G the one before it. Blending by how far we are
    // between them makes the motion continuous even when a simulation step
    // spans many frames — which is exactly the case at low speed. Without this,
    // slowing the water down would make it tick between discrete states.
    // ADVECTION CATCH-UP. The sim advects the whole height field by one step's
    // worth of flow, in a single jump, when it steps — at the bottom of the
    // speed slider that is one lurch per SECOND, and it was the low-speed tick
    // (proven by bisection: disabling this advection removed it, disabling the
    // interpolation below did not). The two height channels are BOTH read at
    // the advected point by the sim, so the pair the mix() blends are already
    // in the same spatial frame as each other. The earlier code slid them in
    // OPPOSITE directions, inventing a displacement between two co-located
    // fields while leaving the real per-step jump uncompensated.
    //
    // Both taps therefore ride the SAME offset: walk the whole field forward
    // by the fraction of a step already elapsed, so by the time the step lands
    // the picture is already exactly where the sim is about to put it and the
    // jump is spent. Live velocity, not a per-step snapshot: what has to match
    // at the boundary is the flow the NEXT step will advect by, and the live
    // field an instant before that step is the best estimate of it.
    vec2 duv = flowShift > 0.0
             ? texture(velTexG, uv).rg * (flowShift * waveSubFrac)
             : vec2(0.0);
    vec2 hh = texture(waveTex, uv - duv).rg - waveBias;
    float h = mix(hh.y, hh.x, waveSubFrac);
    // Instantaneous bow wave of a dragged window: crest hugging the leading
    // edge, trough behind, zero at the centre and far away. Analytic in the
    // window's live position, so it rides with the drag at full frame rate
    // regardless of how slowly the sim steps — in a real fluid the
    // incompressible near-field response IS instantaneous; only shed waves
    // (the sim's job) propagate slowly.
    if (winWake.z > 0.001) {
        vec2  bd  = (uv - winRectSim.xy) / max(winRectSim.zw, vec2(1e-5));
        // Smooth p=4 superellipse metric. max(|x|,|y|) has gradient CREASES
        // along the diagonals, and the caustic pass printed them as a bright
        // X across every dragged window. A p-norm rounds the corners and is
        // smooth everywhere the envelope is nonzero.
        vec2  b2  = bd * bd;
        vec2  b4  = b2 * b2;
        float box = pow(b4.x + b4.y, 0.25);
        // Crest INSIDE the border: the outer falloff ends just past the edge
        // so the displacement never asks the backdrop sampler for pixels far
        // outside its padded region — pushing height right AT the border was
        // resurrecting the old top-edge smear on tall windows.
        float env = smoothstep(1.35, 0.95, box) * smoothstep(0.25, 0.8, box);
        float along = clamp(dot(uv - winRectSim.xy, winWake.xy)
                            / max(length(winRectSim.zw), 1e-5), -1.0, 1.0);
        h += winWake.z * along * env * 0.10;
    }
    // The stroke a drag has deposited since the last simulation step, rendered
    // analytically until the sim absorbs it. The spend zeroes the pending
    // amount in the same frame the deposit lands in the texture, so the
    // handoff is seamless — and slow water responds to dragging continuously
    // at ANY simulation speed, instead of ticking once per rare step. This is
    // the piece sub-stepping alone could not provide at the bottom of the
    // speed slider.
    // One texture read instead of a per-stencil-tap capsule loop: the trail
    // pass already summed every segment this frame, including the
    // just-absorbed one at its crossfade complement. Y-FLIPPED on purpose:
    // the trail FBO's row order is mirrored relative to this shader's uv
    // (verified empirically — a top-edge drag rendered its wake at the
    // bottom until this flip; the sim texture avoids the issue only because
    // it writes and reads itself with the same convention).
    h += texture(trailTex, vec2(uv.x, 1.0 - uv.y)).r;
    return h;
}

// Slope, for pulling the backdrop sample along the surface normal.
vec2 waveSlope(vec2 q, float e) {
    return vec2(waveH(q + vec2(e, 0.0)) - waveH(q - vec2(e, 0.0)),
                waveH(q + vec2(0.0, e)) - waveH(q - vec2(0.0, e))) / (2.0 * e);
}

// Second derivatives (h_xx, h_yy, h_xy) by central differences.
vec3 waveHessian(vec2 q, float e) {
    float c  = waveH(q);
    float xp = waveH(q + vec2(e, 0.0)), xm = waveH(q - vec2(e, 0.0));
    float yp = waveH(q + vec2(0.0, e)), ym = waveH(q - vec2(0.0, e));
    float pp = waveH(q + vec2( e,  e)), mm = waveH(q + vec2(-e, -e));
    float pm = waveH(q + vec2( e, -e)), mp = waveH(q + vec2(-e,  e));
    float e2 = e * e;
    return vec3((xp - 2.0 * c + xm) / e2,
                (yp - 2.0 * c + ym) / e2,
                (pp - pm - mp + mm) / (4.0 * e2));
}

// One caustic layer: brightness = 1/|det(Jacobian)| of the refraction map
// p -> p + k*grad(h). det crosses zero on a CURVE, which is why caustics are
// thin filaments and not blobs.
// Brightness from one area-change factor. Split out so the three wavelengths
// can share a Hessian: the texture reads are the whole cost here, the
// arithmetic is nearly free, so dispersion is close to no extra work at all.
float causticIllum(float det) {
    // ILLUMINATION RELATIVE TO STILL WATER. One quantity, no tuned terms.
    //
    // Refraction maps a patch of surface to a patch of floor,
    // p -> p + k*grad(h), with Jacobian I + k*H. A patch whose area changes by
    // |det| changes its light density by 1/|det|. That is the entire model.
    // Energy conservation is automatic and needs no bookkeeping: the SAME
    // number that brightens a vein dims the water around it, because it is the
    // same patch of light either way. 1.0 means "unchanged".
    //
    // What this replaces was not a model but an accretion - a core clamped at
    // 18, a halo clamped at 5, a dim term weighted 0.16, and a luminance bias -
    // constants tuned against each other with no conserved quantity anywhere.
    float ad = abs(det);
    // The vein is thinner than a pixel nearly everywhere (measured: fwidth(det)
    // exceeds the whole bright band), so point-sampling 1/|det| aliases. Take
    // the pixel AVERAGE instead: for det varying linearly across the pixel that
    // integral is a logarithm. It stays finite on the vein, deposits the vein's
    // true energy into the pixel rather than whatever value the sample happened
    // to land on, and collapses to exactly 1/|det| wherever the vein is
    // properly resolved.
    float w = fwidth(det);
    const float EPS = 0.004;
    return (w > 1.0e-4)
         ? log((ad + 0.5 * w + EPS) / (max(ad - 0.5 * w, 0.0) + EPS)) / w
         : 1.0 / (ad + EPS);
}

// DISPERSION. Water's refractive index is not one number: it bends short
// wavelengths harder than long ones, so blue converges at a shorter distance
// than red. The three colors therefore focus onto slightly DIFFERENT curves,
// and because the veins are thin, curves that miss each other by a few percent
// separate visibly — which is why real caustics carry color along their edges
// instead of being white lines. Deriving it from the same surface is what keeps
// the fringes on the correct side of every vein; tinting the edges by hand puts
// them on whichever side the tint happened to be pushed.
const float DISPERSION = 0.05;

vec3 causticLayer(vec2 q, float e, float lens) {
    // lens here is the SAME displacement coefficient the warp uses, expressed
    // in the wave field's own coordinates. det(I + lens*H) is then literally the
    // area change of the mapping that is bending the image — so a bright vein
    // appears exactly where the warp is squeezing the picture together, instead
    // of being an independent pattern that merely looks similar.
    vec3 H = waveHessian(q, e);
    // Longer wavelength bends less, so red carries the smaller coefficient.
    vec3 k = lens * vec3(1.0 - DISPERSION, 1.0, 1.0 + DISPERSION);
    vec3 det = (1.0 + k * H.x) * (1.0 + k * H.y) - (k * H.z) * (k * H.z);
    return vec3(causticIllum(det.r), causticIllum(det.g), causticIllum(det.b));
}

// THREE SUPERIMPOSED LAYERS.
// Measured against a real pool photo: a single layer, at any density or
// projection distance, never matches it. The photo is several networks at once
// — long swell casts big cells, small ripples cast fine ones, and both project
// simultaneously. Sweeping one layer harder only trades density for sparseness;
// superposition is what produces the overlapping net with bright vertices.
// Each layer gets its own hash seed, so they are independent surfaces rather
// than one surface at three zooms (which would just look like a mip stack).
// Two taps at different finite-difference widths read the SAME simulated
// surface at two scales — the long swell and the fine chop — which is what
// produced the overlapping net in the reference photo.
vec3 caustic(vec2 q, float lensK) {
    // Converted from screen-uv into wave-field units, because the Hessian is
    // measured in the latter. Without this the two effects would agree in
    // principle and disagree in practice.
    float k = lensK * 0.275;
    // Two stencils: the long swell and the chop. The wide one integrates over a
    // longer baseline so it needs a proportionally longer throw to fold at all.
    // ONE LAYER. This used to evaluate the caustic TWICE, at two stencil
    // widths, and combine them - on the reasoning that a real pool shows
    // several networks at once, big cells from the swell and fine ones from
    // the ripples. The observation is right; the implementation was not.
    //
    // Two stencil widths are not two wave systems. They are the SAME surface
    // measured with two different rulers, and they put det's zero crossing in
    // two slightly different places - so every filament was drawn twice, side
    // by side, with a dark gap between the copies. That is what made every
    // wave, every click and every drag come out as a doubled "double bean"
    // separated by a line, at any viscosity and any activity, since it was
    // never a property of the water at all. (Confirmed directly: identical
    // water, two layers vs one - paired parallel curves become single ones.)
    //
    // The multi-scale look has to come from the SURFACE, which genuinely
    // carries both swell and chop, not from measuring it twice. One honest
    // Hessian sees all of it.
    return causticLayer(q, 0.0150, k);
}

vec2 refractionDir(vec2 uv) {
    vec2 toCenterPx = (vec2(0.5) - uv) * fullSize;
    float len = length(toCenterPx);
    return len > 0.1 ? toCenterPx / len : vec2(0.0);
}

// ============================================================================
// MAIN — Thick-glass refraction model
// ============================================================================

void main() {
    vec2 uv = v_texcoord;

    // Layers only: sample the temp FBO to get the rendered surface pixel.
    // Discard fully transparent fragments so glass only covers visible content.
    // For windows, hasMask is false and this block is skipped entirely.
    vec4 surfacePixel = vec4(0.0);
    bool hasMask = (useMask == 1);
    if (hasMask) {
        vec2 maskUV = uv * maskUVScale + maskUVOffset;
        surfacePixel = texture(maskTex, clamp(maskUV, 0.001, 0.999));
        if (surfacePixel.a < maskAlphaThreshold) discard;
    }

    float cornerSdf = getCornerSDF(uv);

    if (cornerSdf > 0.0) {
        discard;
    }

    float cornerAlpha = 1.0 - smoothstep(-1.5, 0.5, cornerSdf);
    if (cornerAlpha < 0.001) discard;

    float minDim = min(fullSize.x, fullSize.y);
    float bezelWidthPx = edgeThickness * minDim;

    // ========================================
    // EDGE PROXIMITY + DIRECTION
    // edgeProximity: 1.0 at boundary, exponential decay inward
    // inwardDir: pixel-space direction toward center (smooth everywhere)
    // ========================================
    float edgeProximity = exp(cornerSdf / bezelWidthPx);
    vec2 inwardDir = refractionDir(uv);

    // ========================================
    // EDGE REFRACTION
    // Offset sampling UV inward (toward center) at edges — like looking
    // through the curved thick edge of a glass slab. This compresses
    // and distorts what's already behind the window, without reaching
    // beyond the window boundary.
    // ========================================
    float refractionPx = refractionStrength * 50.0;
    float refractionMag = edgeProximity * refractionPx;
    vec2 baseOffset = inwardDir * refractionMag / fullSize;

    // ========================================
    // CHROMATIC ABERRATION — per-channel refraction scale
    // Blue refracts more than red → natural spectral fringing at edges.
    // ========================================
    float chromaSpread = chromaticAberration * 0.35;
    vec2 offsetR = baseOffset * (1.0 - chromaSpread);
    vec2 offsetG = baseOffset;
    vec2 offsetB = baseOffset * (1.0 + chromaSpread);

    // ========================================
    // CENTER DOME LENS (subtle magnification in the flat interior)
    // Fades near edges so it doesn't interfere with edge refraction.
    // ========================================
    vec2 domeUV = vec2(0.0);
    if (lensDistortion > 0.001) {
        vec2 c = (uv - 0.5) * 2.0;
        vec2 dGrad = vec2(
            -4.0 * c.x * (1.0 - c.y * c.y),
            -4.0 * c.y * (1.0 - c.x * c.x)
        );
        float lensMaxPx = lensDistortion * minDim * 0.006;
        float lensFade = 1.0 - edgeProximity;
        domeUV = dGrad * lensMaxPx * lensFade / fullSize;
    }

    // ========================================
    // BACKGROUND SAMPLING (frosted blur only)
    // Nearby color influence comes naturally from the Gaussian blur
    // kernel crossing the window boundary — no explicit raw sampling.
    // ========================================
    vec3 color;
    vec2 uvR = uv + offsetR + domeUV;
    vec2 uvG = uv + offsetG + domeUV;
    vec2 uvB = uv + offsetB + domeUV;

    // ========================================
    // LOOKING THROUGH THE WATER
    //
    // The caustics alone are just light drawn on top — bright lines that move.
    // What makes it read as water is that the surface also BENDS what is behind
    // it: a sloped patch of surface displaces the view, so the wallpaper (or
    // whatever window is under this one) visibly warps and swims.
    //
    // Snell's law to first order: the apparent shift is proportional to the
    // surface gradient. Same wave field the caustics come from, so the bright
    // veins and the warping agree with each other rather than being two
    // unrelated effects stacked up.
    //
    // Applied to the SAMPLE positions, before anything is read, so it warps the
    // real backdrop rather than smearing an already-sampled color.
    // ========================================
    vec2  waveWarp = vec2(0.0);
    // The raw surface slope, kept for the Fresnel term below. Free: it is the
    // same value the warp is built from, before depth and aspect scaling.
    vec2  waveGrad = vec2(0.0);
    vec2  wpBase   = vec2(0.0);
    // ONE physical coefficient for both the bending and the brightening.
    // Depth is how far refracted light travels before it lands, so it sets how
    // far the view is displaced AND how much that displacement concentrates the
    // light. Driving them from separate numbers is why depth felt fake: it
    // sharpened the veins without moving anything, which no real depth does.
    // Depth is the physical distance, so it alone sets the coefficient. The
    // warping slider then chooses how much of that bend is actually shown,
    // which is why it can sit at zero without the light going out.
    // Snell's law fixes this; it is not a taste knob. Light leaving water at
    // angle t is bent to n*sin(t), so a surface tilted by a slope shifts what
    // is behind it by depth * (1 - 1/n) * slope. n = 1.333 for water, so the
    // factor is 0.25 and the only free quantity is the depth. There used to be
    // a "warping" slider multiplying this, which amounted to letting you pick
    // the refractive index of water.
    const float WATER_N = 1.333;
    const float SNELL   = 1.0 - 1.0 / WATER_N;
    float lensK    = max(shimmerDepth, 0.10) * SNELL * 0.080;
    if (shimmerIntensity > 0.001) {
        wpBase = 0.5 + ((winOrigin + uvG * winSize) - deskSize * 0.5)
                   / max(deskSize.x, 1.0) * (0.85 * shimmerScale);
        {
            // Divided by the window's pixel size so the shift is a constant
            // number of PIXELS: a small popup and a maximised window then warp
            // by the same visible amount instead of the big one barely moving.
            // Warping scales the whole optical effect; depth is the physical
            // distance. Their product is the displacement coefficient, and the
            // caustics below use the very same number.
            waveGrad = waveSlope(wpBase, 0.0150);
            waveWarp = waveGrad * lensK
                     / max(fullSize / max(fullSize.x, 1.0), vec2(0.001));

            // ── STAY INSIDE THE CAPTURED BACKDROP ──────────────────────────
            // hyprwater only samples a padded region around the window, so
            // there is a hard limit on how far a sample may reach. Past it the
            // sampler clamps, and because the wave keeps moving, the clamped
            // band moves with it — which is what the flicker at the top edge
            // and along a monitor seam actually was. It scaled with warping
            // because the warp is what pushes samples out there.
            //
            // uvPadding is that margin in texture space; convert to window-uv
            // and spend at most part of it, leaving room for the edge
            // refraction that is already using some.
            vec2 margin = uvPadding / max(1.0 - 2.0 * uvPadding, vec2(0.001));
            vec2 budget = max(margin * 0.6, vec2(0.001));
            // SOFT compressor, never a hard clamp. Clamped displacement is
            // CONSTANT displacement: at high depth most of the surface pinned
            // against the budget, and a constant shift moves the image without
            // bending it — straight lines came back the deeper the water got,
            // with hard seams wherever +budget regions met -budget regions.
            // This rational limiter is linear for small warps, approaches the
            // budget asymptotically, and its derivative never reaches zero:
            // deep water compresses its bending but NEVER flattens it.
            waveWarp = waveWarp / (1.0 + abs(waveWarp) / budget);

            // Fade the warp out approaching the border. Even inside the budget,
            // a sample near the edge reaches content the neighbouring monitor
            // captured differently, so the two halves of a spanning window
            // disagree right where they meet. Going to zero at the rim removes
            // the disagreement, and is physically reasonable — the glass is
            // thickest there, and that zone is already dominated by the edge
            // refraction.
            vec2 d = min(uv, 1.0 - uv);                     // distance to nearest edge
            float fade = smoothstep(0.0, 0.10, min(d.x, d.y));
            // Fade near the DESKTOP bounds too. A half-offscreen window's
            // capture is cut at the screen edge — the pixels the warp wants
            // simply were never rendered — and the moving clamp band there
            // flickered with every wave. Nothing valid exists to sample, so
            // the only honest behaviour is calm glass at the screen rim.
            vec2 dp  = winOrigin + uv * winSize;
            vec2 de  = min(dp, deskSize - dp);
            fade *= smoothstep(0.0, 60.0, min(de.x, de.y));
            waveWarp *= fade;
        }
    }
    uvR += waveWarp;
    uvG += waveWarp;
    uvB += waveWarp;

    if (chromaticAberration > 0.001 && edgeProximity > 0.01) {
        color.r = sampleBlurred(uvR).r;
        color.g = sampleBlurred(uvG).g;
        color.b = sampleBlurred(uvB).b;
    } else {
        color = sampleBlurred(uvG).rgb;
    }

    // ========================================
    // FROSTED TINT (per-theme tone mapping)
    // ========================================
    float blurredLum = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Frosted desaturation
    color = mix(vec3(blurredLum), color, saturation);

    // Tight smoothstep range maps the blur-compressed luminance (~0.3-0.7)
    // to the full [0,1] adaptive range, creating visible per-region differentiation
    float lumCurve = smoothstep(0.25, 0.55, blurredLum);

    // Dim: multiplicative — effective at darkening bright areas
    color *= brightness * (1.0 - adaptiveDim * lumCurve);

    // Boost: additive lift — multiplicative can't brighten near-black content
    color += vec3(adaptiveBoost * (1.0 - lumCurve) * 0.5);

    // Contrast (pivot around midpoint)
    color = mix(vec3(0.5), color, contrast);

    // Vibrancy (selective saturation boost scaled by existing saturation)
    float currentLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float darkFactor = 1.0 - vibrancyDarkness * (1.0 - blurredLum);
    color = mix(vec3(currentLum), color, 1.0 + vibrancy * sat * darkFactor);

    // ========================================
    // COLOR TINT OVERLAY
    // ========================================
    color = mix(color, tintColor, tintAlpha);

    // ========================================
    // CAUSTIC SHIMMER — light through a moving surface
    //
    // Added after the tint and before the rim effects so the veins sit IN the
    // glass rather than on top of it, and so fresnel/specular still read as the
    // outermost layer.
    //
    // The brightening is tinted slightly warm and applied against the pixel's
    // own luminance: bright areas take less, so text underneath never gets
    // washed out. Scale is in window-widths rather than pixels, so a small
    // popup and a maximised window show the same size of wave rather than the
    // popup looking like a close-up.
    // ========================================
    if (shimmerIntensity > 0.001) {
        // Sample the water at the REFRACTED coordinate, not the raw one. The
        // glass bends what is behind it; the caustics live in that same water,
        // so they must bend with it. Using plain uv made the pattern ignore the
        // edge entirely and meet the border as a hard line.
        vec2 causticUV = uvG + domeUV;
        // Size-independent mapping, so a popup and a maximised window show the
        // same size of cell rather than one looking like a close-up.
        // Zoom about the CENTRE. Scaling the uv directly scales about (0,0),
        // i.e. the top-left corner, so changing wave size slid the whole pattern
        // toward that corner instead of growing in place.
        vec2 wp = 0.5 + ((winOrigin + causticUV * winSize) - deskSize * 0.5)
                / max(deskSize.x, 1.0) * (0.85 * shimmerScale);
        // One tap, in the same uv domain the wave state uses.
        vec2 cuv = 0.5 + (wp - 0.5) * (2.0 * VIS);
        vec3 c = texture(causticTex, cuv).rgb;

        if (shimmerLightFromBackdrop != 0) {
            // THE FLOOR IS LIT, AND YOU ARE LOOKING AT THE LIT FLOOR.
            //
            // Light refracts through the surface and lands ON the backdrop, so
            // the backdrop is an ALBEDO and the caustic is an ILLUMINATION -
            // and albedo times illumination is a PRODUCT. That single change is
            // what makes bright parts of the wallpaper behave like light coming
            // through the water instead of a pattern laid over the image.
            //
            // It also removes, rather than tunes away, three problems the old
            // additive form had to work around. Nothing can be driven negative,
            // so no clamp to black and no hard-edged shadow contours. Nothing
            // clips to white, so veins keep the backdrop's hue with no
            // max-channel renormalisation. And the illuminant is not the pixel
            // itself, so backdrop detail is modulated once instead of squared -
            // which is what used to stamp a wallpaper's own dither into every
            // filament.
            //
            // The intensity slider is contrast about unity: 0 is still water,
            // 1 is the physical amount, above that is deliberate exaggeration.
            // TONE CURVE. Real caustics out-range any display: a vein can be
            // tens of times the ambient light, and a monitor has about one
            // stop of headroom above white. So the physics is kept exact where
            // it fits and rolled off only where it cannot - a soft knee, not a
            // clip, so bright veins keep their shape and their ordering
            // instead of flattening into a white plateau.
            //
            // The dim side needs no curve: illumination is a ratio, so it is
            // already bounded below by zero. Only the bright side is
            // compressed, which is exactly how a real exposure behaves.
            //
            // GAIN calibrates the slider so its familiar range still lands in
            // the familiar place - the old model buried a 1/18 and a 1/5 in its
            // clamps, and dropping those made the same slider value ~3x hotter.
            const float GAIN     = 0.115;
            // Loosened from 1.6 when the caustic became a SPLAT: the analytic
            // field's singular spikes needed hard compression to fit a
            // display; splatted illumination is bounded by construction, so
            // most of that guard rail was just dimming honest veins.
            const float HEADROOM = 3.0;
            vec3 e = (c - vec3(1.0)) * (shimmerIntensity * GAIN);
            e /= vec3(1.0) + max(e, vec3(0.0)) / HEADROOM;
            color *= max(vec3(1.0) + e, vec3(0.0));
        } else {
            // Independent light: a plain warm source, for when the backdrop is
            // too dark to carry the effect (or a future explicit light).
            float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float headroom = 1.0 - smoothstep(0.55, 1.0, lum);
            // Focusing adds the lamp's own color; spreading can only take away
            // light that is already in the image, so the dim half scales the
            // pixel instead of subtracting a white, which would drive dark
            // backdrops negative.
            // c is ILLUMINATION now, centered at 1.0 (the splat deposits mean
            // one) -- the old analytic path centered it at zero, and reading
            // the new field with the old convention turned the lamp into a
            // permanent veil: max(c, 0) was simply c, everywhere, always.
            vec3 cz = c - vec3(1.0);
            color += vec3(1.0, 0.985, 0.95) * max(cz, vec3(0.0)) * shimmerIntensity * 0.35 * headroom;
            color *= 1.0 + min(cz, vec3(0.0)) * shimmerIntensity * 0.5;
        }
    }

    // ========================================
    // BEER-LAMBERT: WHAT THE WATER ITSELF TAKES OUT
    //
    // Water is not colourless. Pure water absorbs strongly in the red and
    // barely at all in the blue, so light does not just dim with depth, it
    // turns blue -- which is the actual reason the deep ocean is that color,
    // not the sky reflecting off it. Distilled water in a long enough tube does
    // this with no particles involved at all.
    //
    // Coefficients are Pope & Fry (1997) for pure water at 650/550/450nm, in
    // inverse metres, which is what finally makes DEPTH A REAL UNIT: the slider
    // is now metres, and 1.0 means one metre of water. Light goes down to the
    // floor and comes back up to you, so the path is twice the depth.
    //
    // No slider. The absorption of water is not an artistic parameter, and the
    // only honest default for a coefficient is its measured value.
    // ========================================
    if (shimmerIntensity > 0.001) {
        // NOT const: GLSL ES requires a const initialiser to be a constant
        // expression, and a uniform is not one. Writing this as const made the
        // whole shader fail to compile, which the plugin reported once per
        // surface per frame.
        vec3 ABSORB = vec3(0.340, 0.0565, 0.0092) * shimmerAbsorption;
        float path = 2.0 * max(shimmerDepth, 0.0);

        // MURK is scattering rather than absorption, and the two do different
        // things: absorption removes light, scattering REDIRECTS it. Particles
        // both block what is behind them and kick stray light back at you, so
        // murky water loses contrast toward a bright haze instead of just going
        // dark. Large particles scatter every wavelength about equally -- which
        // is why fog and milk are white, not blue -- so the veil is neutral at
        // source and only picks up color from the water it then travels
        // through.
        float b     = shimmerMurk * 2.5;
        vec3  trans = exp(-(ABSORB + vec3(b)) * path);
        // Scattered light averages about half the path before reaching you.
        vec3  veil  = exp(-ABSORB * path * 0.5) * (1.0 - exp(-b * path));

        // SCATTERING MOVES LIGHT SIDEWAYS. That is the whole mechanism, and
        // without it this term was a global multiply plus a constant add --
        // arithmetic identical to a levels adjustment, which is why it read as
        // a gamma filter rather than as water. A particle does not dim the
        // point behind it, it takes light that was heading somewhere else and
        // sends it here, so what you see at any point is a MIX of its
        // surroundings. That spatial mixing is why murk makes things hazy
        // instead of merely pale.
        //
        // The width of that mixing grows with how far light travels through
        // the particles, so it scales with murk AND depth, exactly like the
        // veil does. Four taps on a ring is a crude point-spread function, but
        // it is a spatial one, which the previous version was not at all.
        // In-scattered light has to COME from somewhere. The veil was a fixed
        // magnitude, so murk added glow to a black desktop out of nothing --
        // which is why more of it went whitish-grey instead of dark. Scattering
        // can only redirect light that is already present, so the veil is
        // scaled by how much light is actually around. A dark scene gets a dark
        // haze and keeps going toward black; a bright one goes milky.
        vec3 ambient = color;
        if (b * path > 0.01) {
            float r = min(0.5 * b * path, 6.0) * 0.004;
            vec2  tp = uvG + domeUV;
            vec3  around = sampleBlurred(tp + vec2( r,  r)).rgb
                         + sampleBlurred(tp + vec2(-r,  r)).rgb
                         + sampleBlurred(tp + vec2( r, -r)).rgb
                         + sampleBlurred(tp + vec2(-r, -r)).rgb;
            around *= 0.25;
            ambient = around;
            // How much of what you see arrived by scattering rather than
            // straight through. Same exponential as the veil, because it is
            // the same light.
            float mixed = 1.0 - exp(-b * path * 0.5);
            color = mix(color, around, clamp(mixed, 0.0, 0.85));
        }

        float amb = dot(ambient, vec3(0.2126, 0.7152, 0.0722));
        color = color * trans + veil * amb;
    }

    // ========================================
    // FRESNEL RIM GLOW (edge zone)
    // ========================================
    if (fresnelStrength > 0.001) {
        // FRESNEL FROM THE ACTUAL SURFACE.
        //
        // Water reflects only ~2% of the light hitting it head-on, but the
        // reflectance climbs toward 1 at grazing angles. That single fact is
        // why a droplet on a table is transparent in the middle and shows a
        // bright arc around its rim: the surface there curves away until you
        // are looking along it rather than into it.
        //
        // So the rim glow is not a decoration to be painted on at a fixed
        // strength — it is what happens at any steep piece of surface. Treat
        // BOTH sources of steepness as one slope and run Schlick's
        // approximation on the result:
        //
        //   the meniscus — the droplet's edge curving down to meet the
        //   desktop, which is what edgeProximity has always described; and
        //   the water itself — wave flanks, and above all the bow wave of a
        //   window being dragged, the steepest water on screen.
        //
        // The rim therefore brightens as waves roll into it instead of
        // sitting there, steep water glints wherever it happens to be, and
        // calm water collapses back to exactly the old static rim. The ^5
        // is doing real work as a gate: it ignores gentle swell entirely and
        // only answers to genuinely steep surface, so this picks out features
        // rather than washing the whole window out.
        float rim   = edgeProximity * edgeProximity;
        float slope = length(waveGrad) * 0.85 + rim * 2.0;
        float cosT  = inversesqrt(1.0 + slope * slope);
        float g     = 1.0 - cosT;
        float g2    = g * g;
        float refl  = 0.02 + 0.98 * (g2 * g2 * g);   // Schlick, R0 = 0.02
        color += vec3(1.0) * refl * fresnelStrength * 2.1;
    }

    // ========================================
    // SPECULAR — subtle top highlight (edge zone)
    // ========================================
    if (specularStrength > 0.001) {
        float topBias = pow(max(1.0 - uv.y, 0.0), 2.0);
        float spec = topBias * edgeProximity * edgeProximity * specularStrength * 0.08;
        color += vec3(1.0, 0.99, 0.97) * spec;
    }

    // ========================================
    // INNER SHADOW (bottom rim)
    // ========================================
    {
        float bottomBias = pow(uv.y, 2.0);
        float shadow = bottomBias * edgeProximity * edgeProximity * 0.06;
        color *= 1.0 - shadow;
    }

    float glassA = glassOpacity * cornerAlpha;

    if (hasMask) {
        // Layers only: composite the rendered surface over the glass effect
        // in a single pass. surfacePixel is premultiplied alpha from Hyprland's
        // surface rendering, so we unpremultiply before the 'over' blend.
        float surfA = surfacePixel.a;
        vec3 surfRGB = surfA > 0.001 ? surfacePixel.rgb / surfA : vec3(0.0);

        float compA = surfA + glassA * (1.0 - surfA);
        vec3 compRGB = compA > 0.001
            ? (surfRGB * surfA + color * glassA * (1.0 - surfA)) / compA
            : vec3(0.0);

        // Hyprland's compositor expects premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
        fragColor = vec4(compRGB * compA, compA);
    } else {
        // Windows: output the glass effect alone, surface is rendered separately by Hyprland.
        // Premultiplied: without this, a fading window's glass keeps full RGB contribution
        // because the GL_ONE source factor adds raw color regardless of alpha.
        fragColor = vec4(color * glassA, glassA);
    }
}
)GLSL"},

    {"wavesim.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * WAVE EQUATION STEP  —  d2h/dt2 = c^2 * laplacian(h) - damping
 *
 * WHY THIS EXISTS AT ALL:
 *   Every analytic surface tried before this (plane waves, radial sources,
 *   random-phase Gaussian spectra, multi-scale superposition) shares one fatal
 *   property: it is STATISTICALLY STATIONARY. The statistics are identical at
 *   every point and every moment, forever, so nothing is ever surprised — and
 *   human pattern recognition detects exactly that, no matter how the sum is
 *   dressed up. It is a structural limit of sums of oscillators, not a tuning
 *   problem, which is why six rounds of retuning all read as "patterned".
 *
 *   Integrating the actual wave equation is a different kind of object: a
 *   dynamical system with STATE. A disturbance starts somewhere, spreads at
 *   finite speed, reflects off boundaries, interferes with its own reflections,
 *   and dies out. What happens here now depends on what happened over there
 *   earlier. That history is the thing that cannot be faked with sines.
 *
 * ENCODING: R = h(t), G = h(t-1). Both are signed and stored biased by 0.5 so
 * an ordinary UNORM target can hold negative amplitudes.
 */

uniform sampler2D tex;        // previous state (R = h, G = h_prev)
uniform vec2  texelSize;
uniform float waveSpeed;      // (c*dt/dx)^2 — MUST stay < 0.5 or it explodes
uniform float damping;        // per-step energy retention, slightly below 1
// STROKE SLOTS. A dragged edge does not make a splash: it piles water against
// its advancing face and leaves a trough behind — a dipole, the derivative of
// an elliptical bump along the travel direction, laid along the SEGMENT the
// edge swept. Eight slots so the sim can absorb a whole burst of
// distance-subdivided whip segments in ONE step: segments waiting around
// analytically while the sim's copies flow with the currents was the
// static-vs-flowing handoff chop the user isolated.
uniform vec4  sSeg[8];        // xy = start, zw = end (uv)
uniform vec4  sPar[8];        // xy = direction, z = radius, w = strength
// Second, ROUND-ONLY impulse slot: ambient splashes and click taps, so they
// never contend with the stroke slot above. Ambient amplitude arrives spread
// over several steps (a swell, not a pop) — at low simulation speed a
// one-step splash materialised in ~60 ms while everything else crawled,
// which read as a tick on still water.
uniform vec4  impulse2;       // xy = position, z = radius, w = strength
uniform float volComp;        // uniform counter-volume for clipped deposits
uniform float bedVariation;   // 0 = flat bottom, 1 = strongly uneven
uniform float viscosity;      // how fast SHORT waves die relative to long ones
uniform float maxSpeed;       // largest stable speed for THIS viscosity
// Storage offset. Half float is SIGNED, so on that target this is 0 and the
// height is written raw. Biasing it to sit near 0.5 was costing six bits of
// mantissa: fp16 spacing at 0.5 is 4.9e-4, but at the amplitudes actually being
// stored it is 1.5e-5. The caustic divides its finite differences by e^2, which
// multiplies whatever is in the texture by about 4400 -- so the offset alone
// was manufacturing order-1 curvature out of rounding error and printing it as
// a pixel-scale dither. Only the UNORM fallback, which cannot hold a negative
// number at all, still needs the offset.
uniform float hBias;
// CURRENTS. RG = velocity in uv per simulated second, from the Stable Fluids
// passes. The whole 5-tap stencil below is looked up at the point this texel's
// water CAME FROM, so waves are carried by the flow — a swirl left behind a
// dragged window visibly bends the wavefronts crossing it. flowAdvect is the
// step dt while currents are on and exactly 0 while they are off, so a
// disabled field costs one uniform compare and nothing else.
uniform sampler2D velTex;
uniform float flowAdvect;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// UNEVEN BOTTOM.
// Wave speed in shallow water is c = sqrt(g*h), so a varying depth refracts
// wavefronts: parts of a crest travelling over deeper water outrun parts over
// shallows, and a clean front gradually buckles on its own. Without this a
// wavefront stays a perfect arc until it happens to hit another wave, which is
// exactly the "static as it pans across" look.
//
// This is the RIGHT place to put irregularity: the bed is STATIC and only
// affects propagation speed, so it can never show up as a pattern in the
// caustics the way an irregular surface would. Three sin products, evaluated
// on the 512x512 sim grid only — not per screen pixel — so it is nearly free.
float bedDepth(vec2 p) {
    float d = sin(p.x * 7.3 + 1.7) * sin(p.y * 5.9 - 0.4)
            + 0.55 * sin(p.x * 13.1 - 2.2) * sin(p.y * 11.7 + 1.1)
            + 0.30 * sin((p.x + p.y) * 19.3 + 0.6);
    return d * 0.5;   // roughly -1..1
}

void main() {
    vec2 uv = v_texcoord;
    // Semi-Lagrangian: shift the whole stencil to where this texel's water was
    // one step ago. Both height channels ride together so the wave equation's
    // h/h_prev relationship survives the move.
    if (flowAdvect > 0.0)
        uv -= texture(velTex, uv).rg * flowAdvect;
    vec4 c  = texture(tex, uv);
    float h      = c.r - hBias;
    float hPrev  = c.g - hBias;

    // 9-POINT ISOTROPIC Laplacian. The plain 5-point stencil is anisotropic at
    // short wavelengths — waves travel at different speeds along the axes than
    // along the diagonals — so under heavy agitation, cell-scale bumps get
    // squared off into axis-aligned four-lobed shapes, which the caustic then
    // prints as unnatural 2x2 "flowers" (the user spotted them immediately).
    // The 9-point stencil is isotropic to fourth order; four more taps buys
    // waves that do not know which way the texture grid points. Sampling with
    // clamped edges still makes the boundary a reflecting wall.
    vec4 nL = texture(tex, uv + vec2(-texelSize.x, 0.0));
    vec4 nR = texture(tex, uv + vec2( texelSize.x, 0.0));
    vec4 nD = texture(tex, uv + vec2(0.0, -texelSize.y));
    vec4 nU = texture(tex, uv + vec2(0.0,  texelSize.y));
    vec4 dA = texture(tex, uv + vec2(-texelSize.x, -texelSize.y));
    vec4 dB = texture(tex, uv + vec2( texelSize.x, -texelSize.y));
    vec4 dC = texture(tex, uv + vec2(-texelSize.x,  texelSize.y));
    vec4 dE = texture(tex, uv + vec2( texelSize.x,  texelSize.y));

    float edge = (nL.r + nR.r + nD.r + nU.r) - 4.0 * hBias;
    float corn = (dA.r + dB.r + dC.r + dE.r) - 4.0 * hBias;
    float l    = (4.0 * edge + corn - 20.0 * h) / 6.0;
    // The same Laplacian one step back. It rides along in the .g channel of
    // the samples already taken, so it costs no extra reads.
    float edgeP = (nL.g + nR.g + nD.g + nU.g) - 4.0 * hBias;
    float cornP = (dA.g + dB.g + dC.g + dE.g) - 4.0 * hBias;
    float lp    = (4.0 * edgeP + cornP - 20.0 * hPrev) / 6.0;
    // Stability note: the 9-point operator's eigenvalues span [-16/3, 0]
    // instead of the 5-point's [-8, 0], so every speed the CPU-side ceiling
    // (derived for the 5-point) allows is strictly INSIDE the stable region
    // here — verified numerically (scratchpad sweep: ceiling 0.75 - 2v vs the
    // 5-point's ~0.49 - 2v). Speeds are deliberately unchanged.

    // Explicit second-order integration: h(t+1) = 2h - h(t-1) + c^2 * lap
    // Local propagation speed from the local depth. Clamped well under the
    // Courant limit (0.5) at the FAST end, or the scheme diverges wherever the
    // water is deepest.
    // STABILITY. The explicit scheme needs c^2 + nu < 0.5 at the highest
    // representable frequency, so the ceiling on the local speed has to come
    // DOWN as viscosity goes up -- they share one budget. Pinning the bed to
    // fully uneven already pushed far more of the surface up against this
    // ceiling, and adding viscosity on top of that tipped it over: the shortest
    // wavelengths then amplify instead of decaying, which shows up as growing
    // blobs in the corners where reflections pile up.
    // Ceiling solved on the CPU from the viscosity: the two are bound by
    // s + v < sqrt(s/2), which is far stricter than the familiar s < 0.5.
    // Expressed as a FRACTION of the ceiling rather than an absolute speed that
    // then gets clipped. Written the old way, most of the surface sat pinned at
    // exactly the maximum, so the uneven bottom stopped varying anything over
    // the majority of the area -- and an uneven bottom that is uniform in
    // practice is what lets wavefronts stay perfect arcs. This way the whole
    // range lives under the limit no matter how thick the water is set.
    // v_texcoord, not the advected uv: the bed is the FLOOR. It does not flow
    // with the water above it.
    float localSpeed = maxSpeed * waveSpeed
                     * (0.65 + 0.35 * bedVariation * bedDepth(v_texcoord));
    localSpeed = clamp(localSpeed, 0.01, maxSpeed);

    // VISCOSITY. A single multiplicative damping factor removes the same
    // fraction of every wavelength, so the fine chop survives exactly as long
    // as the long swell does -- the surface keeps a permanent high-frequency
    // jitter and never resolves into anything, which is why it read as frozen
    // rather than liquid. Real water dissipates as nu*k^2: short waves lose
    // energy far faster than long ones, which is precisely why a disturbed pool
    // settles into smooth slow swells instead of staying grainy.
    //
    // nu * laplacian(dh/dt) is that term. It leaves the swell almost untouched
    // and eats the pixel-scale ripple within a few steps. Kept well under the
    // 0.25 explicit-diffusion stability bound.
    // DAMPING PLACEMENT. Multiplying the WHOLE update by the retention factor
    // looks like a drag and is not one: expanding d*(2h - hPrev + ...) leaves
    // a -(1-d)*h term, a restoring force on DISPLACEMENT -- a Klein-Gordon
    // mass. A massive medium is dispersive (w^2 = w0^2 + c^2 k^2), so every
    // pulse grew an oscillating tail: a locked crest+trough pair at constant
    // separation that never passes itself, on every click, drag and ambient
    // splash alike. Verified against an exact spectral integrator: with the
    // mass term gone the same kick is one clean crest. Damping only the
    // VELOCITY (h - hPrev) is a pure drag -- the per-mode decay is sqrt(d)
    // either way (the root product is d in both forms), so the settle pacing
    // is untouched; only the phantom second wavefront goes.
    float hNext = h + (h - hPrev) * damping + localSpeed * l + viscosity * (l - lp);

    // Stroke deposits — up to eight segments per step (see the uniform note).
    // v_texcoord, not the advected uv: the thing pushing is an external
    // object at a fixed place, not a parcel of the flowing sheet.
    for (int si = 0; si < 8; si++) {
        if (sPar[si].w == 0.0) continue;
        vec2  A  = sSeg[si].xy;
        vec2  AB = sSeg[si].zw - A;
        float l2 = dot(AB, AB);
        float ra = max(sPar[si].z, 1e-9);
        if (dot(sPar[si].xy, sPar[si].xy) < 0.5) {
            // Round splash (no direction): crater + rim at the segment start.
            // Difference of Gaussians rather than a plain bump, for the same
            // reason the drag below is a dipole: an impact DISPLACES water,
            // it does not create any, so the crater's volume must come back
            // up in a rim. A plain bump pumps net volume into the pool --
            // nothing restores the mean now that damping is a pure drag --
            // and its spectrum reaches down to domain-scale modes, which
            // ring far too hard without the old mass term stiffening them.
            // The DoG's spectrum vanishes at DC and rolls off exactly that
            // band; its rim is 4x shallower and 2x wider than the crest,
            // well below what the caustic will draw.
            vec2  q = (v_texcoord - A) * vec2(1.0, texelSize.x / max(texelSize.y, 1e-6));
            float d = length(q);
            hNext += (exp(-(d * d) / (ra * ra))
                      - 0.25 * exp(-(d * d) / (4.0 * ra * ra))) * sPar[si].w;
        } else {
            float t  = l2 > 1e-12 ? clamp(dot(v_texcoord - A, AB) / l2, 0.0, 1.0) : 0.0;
            vec2  r  = v_texcoord - (A + t * AB);
            float along  = dot(r, sPar[si].xy);
            float across = dot(r, vec2(-sPar[si].y, sPar[si].x));
            float rb = ra * 3.5;
            float e  = exp(-(along * along) / (ra * ra)
                           - (across * across) / (rb * rb));
            // d/d(along): positive ahead, negative behind, integrates to
            // zero — a drag displaces water rather than adding any.
            hNext += sPar[si].w * (along / ra) * e * 2.0;
        }
    }

    if (impulse2.w != 0.0) {
        // Crater + rim, same DoG as the round stroke branch above.
        vec2  q2 = (v_texcoord - impulse2.xy)
                 * vec2(1.0, texelSize.x / max(texelSize.y, 1e-6));
        float d2 = length(q2);
        float r3 = max(impulse2.z, 1e-9);
        hNext += (exp(-(d2 * d2) / (r3 * r3))
                  - 0.25 * exp(-(d2 * d2) / (4.0 * r3 * r3))) * impulse2.w;
    }
    // Uniform counter-volume for this step's round deposits. The DoG is
    // volume-neutral only while it fits inside the domain: an event near the
    // wall writes only the part of itself that lands on texels, and the
    // clipped remainder would otherwise accumulate as a permanent offset of
    // the mean (there is no restoring force to bleed it away). The CPU knows
    // the clipped fraction exactly -- erf over the in-domain box -- and
    // spreads the correction uniformly here. A constant has no gradient, so
    // nothing downstream can see it.
    hNext -= volComp;

    // SOFT amplitude limit. The old hard clamp at 0.49 flat-topped the waves
    // whenever agitation outran damping — and a plateau's rim is a ring of
    // enormous curvature that the caustic pass faithfully printed as harsh
    // outlines all over the surface (measured: at max agitation 0.7% of ALL
    // texels sat on the clamp every step; with this limiter, none do — the
    // asymptote is never reached). Linear below 0.30, exponential approach to
    // 0.49 above it, C1-continuous at the knee: waves compress like water
    // instead of clipping like audio.
    float aa = abs(hNext);
    if (aa > 0.30)
        hNext = sign(hNext) * (0.30 + 0.19 * (1.0 - exp(-(aa - 0.30) / 0.19)));
    fragColor = vec4(hNext + hBias, h + hBias, 0.0, 1.0);
}
)GLSL"},

/*
 * CURRENTS — Stam's Stable Fluids, four passes over a 256x256 velocity field.
 *
 * The wave equation above is a SCALAR height field: vorticity (curl v) is
 * undefined on it — you cannot curl a scalar — so no amount of tuning it will
 * ever produce an eddy. Eddies need a velocity field. These passes maintain
 * one: advect it by itself, push it where a window dragged, then project it
 * back to divergence-free. The projection IS mass conservation: a force can
 * move water around but can never create or destroy it, which is what makes
 * permeable window edges safe — overlapping windows are two viewports onto the
 * same fluid, nothing is ever trapped or compressed.
 *
 * Convention shared by all four passes and the numpy prototype they were
 * verified against: velocity in uv per SIMULATED SECOND, dx = one texel in uv,
 * all derivatives central differences on clamped edges.
 * Divergence solves  lap(q) = div(v)  and then  v -= grad(q); the residual a
 * 24-iteration Jacobi leaves is texel-scale checkerboard (the collocated-grid
 * null space), which the 4x bilinear upsample into the wave grid filters out.
 */
    {"fluid_advect.frag", R"GLSL(
#version 300 es
precision highp float;

// PASS 1: SEMI-LAGRANGIAN ADVECTION + FORCE + DISSIPATION.
// "Where did the fluid at this texel come from?" — walk backward along the
// local velocity and take what was there. Unconditionally stable at any speed
// (it interpolates history rather than extrapolating a derivative), at the
// price of being slightly diffusive — which for water read through a caustic
// is a feature, not a bug.
uniform sampler2D tex;      // velocity, RG = (vx, vy) in uv/second, signed
uniform float dt;
uniform float dissipation;  // per-step momentum retention, slightly below 1
uniform vec4  force;        // xy = position (uv), z = radius, w = impulse (uv/s at peak)
uniform vec2  forceDir;     // unit direction of the push

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = v_texcoord;
    vec2 v0 = texture(tex, uv).rg;
    vec2 v  = texture(tex, uv - v0 * dt).rg * dissipation;
    // A dragged window edge is momentum arriving in the water — this splat is
    // the real thing the height dipole approximates. The projection pass will
    // reshape it into a divergence-free swirl: a vortex pair at the ends of
    // the swept region, which is exactly the corner-whirlpool look.
    if (force.w != 0.0) {
        vec2 d = uv - force.xy;
        v += forceDir * force.w * exp(-dot(d, d) / (force.z * force.z));
    }
    fragColor = vec4(v, 0.0, 1.0);
}
)GLSL"},

    {"fluid_divergence.frag", R"GLSL(
#version 300 es
precision highp float;

// PASS 2: DIVERGENCE of the advected velocity — how much each cell is being
// pumped into or drained, per second. This is what the pressure solve must
// cancel.
uniform sampler2D tex;      // velocity
uniform vec2 texelSize;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = v_texcoord;
    float vL = texture(tex, uv - vec2(texelSize.x, 0.0)).r;
    float vR = texture(tex, uv + vec2(texelSize.x, 0.0)).r;
    float vD = texture(tex, uv - vec2(0.0, texelSize.y)).g;
    float vU = texture(tex, uv + vec2(0.0, texelSize.y)).g;
    fragColor = vec4((vR - vL + vU - vD) / (2.0 * texelSize.x), 0.0, 0.0, 1.0);
}
)GLSL"},

    {"fluid_jacobi.frag", R"GLSL(
#version 300 es
precision highp float;

// PASS 3 (iterated): one Jacobi relaxation of  lap(q) = div. Information moves
// one texel per iteration, so 24 iterations cannot flatten a domain-sized mode
// in one frame — but q is WARM-STARTED from the previous step, so across a few
// frames the effective iteration count is whatever it needs to be.
uniform sampler2D tex;      // q from the previous iteration
uniform sampler2D divTex;   // divergence, fixed for the whole solve
uniform vec2 texelSize;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = v_texcoord;
    float qL = texture(tex, uv - vec2(texelSize.x, 0.0)).r;
    float qR = texture(tex, uv + vec2(texelSize.x, 0.0)).r;
    float qD = texture(tex, uv - vec2(0.0, texelSize.y)).r;
    float qU = texture(tex, uv + vec2(0.0, texelSize.y)).r;
    float d  = texture(divTex, uv).r;
    fragColor = vec4((qL + qR + qD + qU - d * texelSize.x * texelSize.x) * 0.25,
                     0.0, 0.0, 1.0);
}
)GLSL"},

    {"fluid_gradient.frag", R"GLSL(
#version 300 es
precision highp float;

// PASS 4: subtract grad(q) — remove the compressive part of the field and
// keep the swirl. What survives this pass is by construction divergence-free:
// pure rotation and shear, the only motions incompressible water can do.
uniform sampler2D tex;      // velocity
uniform sampler2D prsTex;   // solved q
uniform vec2 texelSize;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = v_texcoord;
    float qL = texture(prsTex, uv - vec2(texelSize.x, 0.0)).r;
    float qR = texture(prsTex, uv + vec2(texelSize.x, 0.0)).r;
    float qD = texture(prsTex, uv - vec2(0.0, texelSize.y)).r;
    float qU = texture(prsTex, uv + vec2(0.0, texelSize.y)).r;
    vec2 v = texture(tex, uv).rg
           - vec2(qR - qL, qU - qD) / (2.0 * texelSize.x);
    fragColor = vec4(v, 0.0, 1.0);
}
)GLSL"},

    {"trail.frag", R"GLSL(
#version 300 es
precision highp float;

// THE ANALYTIC STROKE TRAIL, summed once per FRAME into a texture the glass
// shader reads with a single tap. Strokes are subdivided by DISTANCE as the
// window moves, so this is a fine polyline of the drag path no matter how
// slowly the simulation itself is stepping — evaluating ~26 capsules per
// texel once per frame here is far cheaper than doing it inside every
// stencil tap of the caustic pass, and it puts no ceiling on trail length.
uniform vec4 tSeg[26];   // xy = start, zw = end (sim uv)
uniform vec4 tPar[26];   // xy = direction, z = radius, w = amplitude

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2  uv = v_texcoord;
    float h  = 0.0;
    for (int i = 0; i < 26; i++) {
        if (tPar[i].w == 0.0) continue;
        vec2  A  = tSeg[i].xy;
        vec2  AB = tSeg[i].zw - A;
        float l2 = dot(AB, AB);
        float t  = l2 > 1e-12 ? clamp(dot(uv - A, AB) / l2, 0.0, 1.0) : 0.0;
        vec2  r  = uv - (A + t * AB);
        float alo = dot(r, tPar[i].xy);
        float acr = dot(r, vec2(-tPar[i].y, tPar[i].x));
        float ra  = max(tPar[i].z, 1e-5);
        float rb  = ra * 3.5;
        h += tPar[i].w * (alo / ra)
           * exp(-(alo * alo) / (ra * ra) - (acr * acr) / (rb * rb)) * 2.0;
    }
    fragColor = vec4(h, 0.0, 0.0, 1.0);
}
)GLSL"},

    {"causticsplat.vert", R"GLSL(
#version 300 es
precision highp float;

// ============================================================================
// FORWARD-SPLAT CAUSTIC — the vertex shader IS the optics.
//
// The analytic 1/|det(I + k*H)| formulation asks "how much did the patch
// arriving HERE spread?", and past a fold that question has several answers
// of which the formula only ever sees one. That is exactly why the two
// astigmatic focal lines of every bump rendered as two separate bright rings,
// why the field's mean drifted with the fold population (measured 2.2 where
// energy conservation demands 1.0), and why blurring that saturated field
// washed the image out.
//
// Splatting asks the question light actually answers: each surface patch
// carries a fixed parcel of energy to WHEREVER refraction lands it. One
// vertex per texel of the caustic target samples the surface slope, emits
// gl_Position at the refracted landing point, and additive blending sums the
// arrivals. Overlaps ADD — the two folds of a cusp pile into one bright line
// instead of being drawn twice — total energy is conserved by construction,
// and still water deposits exactly 1.0 in every texel. No singularity, no
// doubling, nothing to renormalize.
// ============================================================================

uniform sampler2D tex;        // wave state: R = h(t), G = h(t-1)
uniform sampler2D trailTex;   // analytic stroke trail, same uv domain
uniform sampler2D velTexG;    // velocity field, for advected interpolation
uniform float waveSubFrac;
uniform float waveBias;
uniform float flowShift;
uniform float causticK;       // lens coefficient (depth * Snell * scale)
uniform float gridN;          // points per side == target texels per side

flat out vec2 vLand;          // landing point, target pixel coords

const float VIS = 0.105;

float waveH(vec2 q) {
    vec2 uv = 0.5 + (q - 0.5) * (2.0 * VIS);
    vec2 duv = flowShift > 0.0
             ? texture(velTexG, uv).rg * (flowShift * waveSubFrac)
             : vec2(0.0);
    vec2 hh = texture(tex, uv - duv).rg - waveBias;
    float h = mix(hh.y, hh.x, waveSubFrac);
    h += texture(trailTex, vec2(uv.x, 1.0 - uv.y)).r;
    return h;
}

void main() {
    float fid = float(gl_VertexID);
    float iy  = floor(fid / gridN);
    float ix  = fid - iy * gridN;
    // One point per target texel, on the plain lattice. With the BILINEAR
    // deposit below, still water lands exactly 1.0 in every texel -- no
    // counting noise at all. (Jitter was tried here twice to fight a woven
    // lattice moire seen at the original lens strength: white noise traded
    // the weave for connected mottle, low-discrepancy-by-linear-index
    // aliased into curtains. At vein-regime lens strength the displacements
    // are a few texels and the plain lattice shows no weave to begin with.)
    vec2 tuv  = (vec2(ix, iy) + 0.5) / gridN;

    // Same q-space as the glass shader's warp, so the light lands exactly
    // where the warped image says it moved.
    vec2 q = 0.5 + (tuv - 0.5) / (2.0 * VIS);

    // Slope by central differences. A tighter arm than the warp's Hessian
    // stencil: the deposit's fold sharpness is set by this smoothing, and a
    // texel of positional disagreement with the warp is invisible while a
    // 3-texel smear of every vein is not.
    const float e = 0.0150;
    vec2 grad = vec2(waveH(q + vec2(e, 0.0)) - waveH(q - vec2(e, 0.0)),
                     waveH(q + vec2(0.0, e)) - waveH(q - vec2(0.0, e)))
              / (2.0 * e);

    vec2 pq   = q + causticK * grad;
    vec2 ptex = 0.5 + (pq - 0.5) * (2.0 * VIS);
    gl_Position  = vec4(ptex * 2.0 - 1.0, 0.0, 1.0);
    // BILINEAR deposit, not nearest-pixel. Rounding each parcel into one
    // texel quantizes the density field: where the mapping spreads light thin
    // the counts land as 0-or-1 per texel, which is 100% relative noise, and
    // it showed as grit over every dim region. A 2x2 point plus the bilinear
    // weight in the fragment stage deposits the parcel EXACTLY, so still
    // water is exactly 1.0 everywhere and density varies smoothly.
    vLand        = ptex * gridN;
    gl_PointSize = 2.0;
}
)GLSL"},

    {"causticsplat.frag", R"GLSL(
#version 300 es
precision highp float;

// One parcel of light per point, split bilinearly over the 2x2 pixels the
// point covers; GL_ONE/GL_ONE blending does the physics — arrivals sum.
// Alpha stays 0 so the cleared 1.0 survives.
flat in vec2 vLand;
layout(location = 0) out vec4 fragColor;

void main() {
    float w = max(0.0, 1.0 - abs(gl_FragCoord.x - vLand.x))
            * max(0.0, 1.0 - abs(gl_FragCoord.y - vLand.y));
    fragColor = vec4(w, w, w, 0.0);
}
)GLSL"},

    {"gaussianblur.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 direction; // (1.0/width, 0.0) for horizontal, (0.0, 1.0/height) for vertical
uniform float blurRadius; // kernel radius in pixels

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    // Compute sigma from radius (covers ~3 sigma)
    float sigma = max(blurRadius / 3.0, 0.001);
    float invSigma2 = -0.5 / (sigma * sigma);

    int samples = min(int(ceil(blurRadius)), 8);

    // Center tap
    float w0 = 1.0;
    vec4 result = texture(tex, v_texcoord) * w0;
    float totalWeight = w0;

    // Linear sampling: pair adjacent taps (i, i+1) into a single bilinear fetch.
    // The interpolated offset between two texels yields their weighted average
    // in one texture() call, halving the total tap count.
    for (int i = 1; i <= samples; i += 2) {
        float x1 = float(i);
        float x2 = float(i + 1);
        float w1 = exp(x1 * x1 * invSigma2);
        float w2 = (i + 1 <= samples) ? exp(x2 * x2 * invSigma2) : 0.0;
        float wSum = w1 + w2;
        if (wSum < 0.0001) continue;

        // Offset biased toward the heavier weight
        float offset = (x1 * w1 + x2 * w2) / wSum;

        result += texture(tex, v_texcoord + direction * offset) * wSum;
        result += texture(tex, v_texcoord - direction * offset) * wSum;
        totalWeight += 2.0 * wSum;
    }

    fragColor = result / totalWeight;
}
)GLSL"},
};
