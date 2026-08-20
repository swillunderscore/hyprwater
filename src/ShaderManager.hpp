#pragma once

#include <GLES3/gl32.h>
#include <hyprland/src/render/Shader.hpp>
#include <string>

struct SGlassUniforms {
    GLint refractionStrength = -1;
    GLint uTime             = -1;
    GLint shimmerIntensity  = -1;
    GLint shimmerSpeed      = -1;
    GLint shimmerScale      = -1;
    GLint shimmerLightFromBackdrop = -1;
    GLint waveTex                  = -1;
    GLint shimmerDepth             = -1;
    GLint waveSubFrac              = -1;
    GLint waveTexel                = -1;
    GLint winWake                  = -1;
    GLint winRectSim               = -1;
    GLint trailTex                 = -1;
    GLint causticTex               = -1;
    GLint velTexG                  = -1;
    GLint flowShift                = -1;
    GLint waveBias                 = -1;
    GLint shimmerMurk              = -1;
    GLint shimmerAbsorption        = -1;
    GLint winOrigin                = -1;
    GLint winSize                  = -1;
    GLint deskSize                 = -1;
    GLint chromaticAberration = -1;
    GLint fresnelStrength = -1;
    GLint specularStrength = -1;
    GLint glassOpacity = -1;
    GLint edgeThickness = -1;
    GLint uvPadding = -1;
    GLint tintColor = -1;
    GLint tintAlpha = -1;
    GLint adaptiveTint = -1;
    GLint adaptiveTarget = -1;
    GLint adaptiveLumaTex = -1;
    GLint lensDistortion = -1;
    GLint saturation = -1;
    GLint brightness = -1;
    GLint contrast   = -1;
    GLint vibrancy   = -1;
    GLint vibrancyDarkness = -1;
    GLint adaptiveDim = -1;
    GLint adaptiveBoost = -1;
    
    // Layers only: temp FBO surface mask for content-aware glass
    GLint maskTex = -1;
    GLint useMask = -1;
    GLint maskUVOffset = -1;
    GLint maskUVScale = -1;
    GLint maskAlphaThreshold = -1;
};

struct SAdaptiveLumaUniforms {
    GLint prevTex   = -1;
    GLint emaAlpha  = -1;
    GLint seedPrev  = -1;
};

struct SBlurUniforms {
    GLint direction = -1;
    GLint radius    = -1;
};

struct SWaveSimUniforms {
    GLint texelSize = -1;
    GLint waveSpeed = -1;
    GLint damping   = -1;
    GLint sSeg       = -1;
    GLint sPar       = -1;
    GLint impulse2   = -1;
    GLint volComp    = -1;
    GLint bedVariation = -1;
    GLint viscosity    = -1;
    GLint maxSpeed     = -1;
    GLint hBias        = -1;
    GLint velTex       = -1;
    GLint flowAdvect   = -1;
};

// One struct for all four Stable Fluids passes, prefixed by pass:
// a = advect, d = divergence, j = jacobi, g = gradient.
struct STrailUniforms {
    GLint tSeg = -1;
    GLint tPar = -1;
};

struct SCausticUniforms {
    GLint trailTex    = -1;
    GLint velTexG     = -1;
    GLint waveSubFrac = -1;
    GLint waveBias    = -1;
    GLint gridN       = -1;
    GLint flowShift   = -1;
    GLint causticK    = -1;
};

struct SFluidUniforms {
    GLint aDt          = -1;
    GLint aDissipation = -1;
    GLint aForce       = -1;
    GLint aForceDir    = -1;
    GLint dTexelSize   = -1;
    GLint jTexelSize   = -1;
    GLint jDivTex      = -1;
    GLint gTexelSize   = -1;
    GLint gPrsTex      = -1;
};

class CShaderManager {
  public:
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    void initializeIfNeeded();
    void destroy() noexcept;

    SP<CShader>    glassShader = makeShared<CShader>();
    SGlassUniforms glassUniforms;

    SP<CShader>    blurShader = makeShared<CShader>();
    SBlurUniforms  blurUniforms;

    SP<CShader>           adaptiveLumaShader = makeShared<CShader>();
    SAdaptiveLumaUniforms adaptiveLumaUniforms;

    SP<CShader>      waveSimShader = makeShared<CShader>();
    SWaveSimUniforms waveSimUniforms;

    SP<CShader>    trailShader = makeShared<CShader>();
    STrailUniforms trailUniforms;

    SP<CShader>      causticShader = makeShared<CShader>();
    SCausticUniforms causticUniforms;

    SP<CShader>    fluidAdvectShader     = makeShared<CShader>();
    SP<CShader>    fluidDivergenceShader = makeShared<CShader>();
    SP<CShader>    fluidJacobiShader     = makeShared<CShader>();
    SP<CShader>    fluidGradientShader   = makeShared<CShader>();
    SFluidUniforms fluidUniforms;

  private:
    bool m_initialized = false;

    [[nodiscard]] static std::string loadShaderSource(const char* fileName);
    [[nodiscard]] bool compileGlassShader();
    [[nodiscard]] bool compileBlurShader();
    [[nodiscard]] bool compileAdaptiveLumaShader();
    [[nodiscard]] bool compileWaveSimShader();
    [[nodiscard]] bool compileFluidShaders();
    [[nodiscard]] bool compileTrailShader();
    [[nodiscard]] bool compileCausticShader();
};
