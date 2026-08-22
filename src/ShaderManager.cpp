#include "ShaderManager.hpp"
#include "Globals.hpp"
#include "Shaders.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>

std::string CShaderManager::loadShaderSource(const char* fileName) {
    if (SHADERS.contains(fileName))
        return SHADERS.at(fileName);

    const std::string message = std::format("[{}] Failed to load shader: {}", PLUGIN_NAME, fileName);
    HyprlandAPI::addNotification(PHANDLE, message, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error(message);
}

bool CShaderManager::compileGlassShader() {
    if (!glassShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("liquidglass.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile glass shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = glassShader->program();

    glassUniforms.refractionStrength  = glGetUniformLocation(program, "refractionStrength");
    glassUniforms.uTime               = glGetUniformLocation(program, "uTime");
    glassUniforms.shimmerIntensity    = glGetUniformLocation(program, "shimmerIntensity");
    glassUniforms.shimmerSpeed        = glGetUniformLocation(program, "shimmerSpeed");
    glassUniforms.shimmerScale        = glGetUniformLocation(program, "shimmerScale");
    glassUniforms.shimmerLightFromBackdrop = glGetUniformLocation(program, "shimmerLightFromBackdrop");
    glassUniforms.waveTex                  = glGetUniformLocation(program, "waveTex");
    glassUniforms.shimmerDepth             = glGetUniformLocation(program, "shimmerDepth");
    glassUniforms.waveSubFrac              = glGetUniformLocation(program, "waveSubFrac");
    glassUniforms.waveTexel                = glGetUniformLocation(program, "waveTexel");
    glassUniforms.waveSmoothTex            = glGetUniformLocation(program, "waveSmoothTex");
    glassUniforms.waveSmoothTexel          = glGetUniformLocation(program, "waveSmoothTexel");
    glassUniforms.causticK                 = glGetUniformLocation(program, "causticK");
    glassUniforms.winWake                  = glGetUniformLocation(program, "winWake");
    glassUniforms.winRectSim               = glGetUniformLocation(program, "winRectSim");
    glassUniforms.trailTex                 = glGetUniformLocation(program, "trailTex");
    glassUniforms.causticTex               = glGetUniformLocation(program, "causticTex");
    glassUniforms.velTexG                  = glGetUniformLocation(program, "velTexG");
    glassUniforms.flowShift                = glGetUniformLocation(program, "flowShift");
    glassUniforms.waveBias           = glGetUniformLocation(program, "waveBias");
    glassUniforms.shimmerMurk        = glGetUniformLocation(program, "shimmerMurk");
    glassUniforms.shimmerAbsorption  = glGetUniformLocation(program, "shimmerAbsorption");
    glassUniforms.winOrigin          = glGetUniformLocation(program, "winOrigin");
    glassUniforms.winSize            = glGetUniformLocation(program, "winSize");
    glassUniforms.deskSize           = glGetUniformLocation(program, "deskSize");
    glassUniforms.chromaticAberration = glGetUniformLocation(program, "chromaticAberration");
    glassUniforms.fresnelStrength     = glGetUniformLocation(program, "fresnelStrength");
    glassUniforms.specularStrength    = glGetUniformLocation(program, "specularStrength");
    glassUniforms.glassOpacity        = glGetUniformLocation(program, "glassOpacity");
    glassUniforms.edgeThickness       = glGetUniformLocation(program, "edgeThickness");
    glassUniforms.uvPadding           = glGetUniformLocation(program, "uvPadding");
    glassUniforms.tintColor           = glGetUniformLocation(program, "tintColor");
    glassUniforms.tintAlpha           = glGetUniformLocation(program, "tintAlpha");
    glassUniforms.adaptiveTint         = glGetUniformLocation(program, "adaptiveTint");
    glassUniforms.adaptiveTarget       = glGetUniformLocation(program, "adaptiveTarget");
    glassUniforms.adaptiveLumaTex      = glGetUniformLocation(program, "adaptiveLumaTex");
    glassUniforms.lensDistortion      = glGetUniformLocation(program, "lensDistortion");
    glassUniforms.saturation          = glGetUniformLocation(program, "saturation");
    glassUniforms.brightness          = glGetUniformLocation(program, "brightness");
    glassUniforms.contrast            = glGetUniformLocation(program, "contrast");
    glassUniforms.vibrancy            = glGetUniformLocation(program, "vibrancy");
    glassUniforms.vibrancyDarkness    = glGetUniformLocation(program, "vibrancyDarkness");
    glassUniforms.adaptiveDim         = glGetUniformLocation(program, "adaptiveDim");
    glassUniforms.adaptiveBoost       = glGetUniformLocation(program, "adaptiveBoost");
    glassUniforms.maskTex             = glGetUniformLocation(program, "maskTex");
    glassUniforms.useMask             = glGetUniformLocation(program, "useMask");
    glassUniforms.maskUVOffset        = glGetUniformLocation(program, "maskUVOffset");
    glassUniforms.maskUVScale         = glGetUniformLocation(program, "maskUVScale");
    glassUniforms.maskAlphaThreshold  = glGetUniformLocation(program, "maskAlphaThreshold");

    return true;
}

bool CShaderManager::compileAdaptiveLumaShader() {
    if (!adaptiveLumaShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("adaptiveluma.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile adaptive luma shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }
    const auto program = adaptiveLumaShader->program();
    adaptiveLumaUniforms.prevTex  = glGetUniformLocation(program, "prevTex");
    adaptiveLumaUniforms.emaAlpha = glGetUniformLocation(program, "emaAlpha");
    adaptiveLumaUniforms.seedPrev = glGetUniformLocation(program, "seedPrev");
    return true;
}

bool CShaderManager::compileBlurShader() {
    if (!blurShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("gaussianblur.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile blur shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = blurShader->program();

    blurUniforms.direction = glGetUniformLocation(program, "direction");
    blurUniforms.radius    = glGetUniformLocation(program, "blurRadius");

    return true;
}

bool CShaderManager::compileWaveSimShader() {
    if (!waveSimShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("wavesim.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile wave sim shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = waveSimShader->program();

    waveSimUniforms.texelSize = glGetUniformLocation(program, "texelSize");
    waveSimUniforms.waveSpeed = glGetUniformLocation(program, "waveSpeed");
    waveSimUniforms.damping   = glGetUniformLocation(program, "damping");
    waveSimUniforms.sSeg     = glGetUniformLocation(program, "sSeg[0]");
    waveSimUniforms.sPar     = glGetUniformLocation(program, "sPar[0]");
    waveSimUniforms.impulse2 = glGetUniformLocation(program, "impulse2");
    waveSimUniforms.volComp  = glGetUniformLocation(program, "volComp");
    waveSimUniforms.bedVariation = glGetUniformLocation(program, "bedVariation");
    waveSimUniforms.viscosity    = glGetUniformLocation(program, "viscosity");
    waveSimUniforms.maxSpeed     = glGetUniformLocation(program, "maxSpeed");
    waveSimUniforms.hBias        = glGetUniformLocation(program, "hBias");
    waveSimUniforms.velTex       = glGetUniformLocation(program, "velTex");
    waveSimUniforms.flowAdvect   = glGetUniformLocation(program, "flowAdvect");

    return true;
}

bool CShaderManager::compileTrailShader() {
    if (!trailShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("trail.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile trail shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }
    const auto program = trailShader->program();
    trailUniforms.tSeg = glGetUniformLocation(program, "tSeg[0]");
    trailUniforms.tPar = glGetUniformLocation(program, "tPar[0]");
    return true;
}

bool CShaderManager::compileCausticShader() {
    // Custom VERTEX shader: the splat's work happens per vertex, the fragment
    // stage only deposits the parcel it was handed.
    if (!causticShader->createProgram(
            loadShaderSource("causticsplat.vert"),
            loadShaderSource("causticsplat.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile caustic shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }
    const auto program = causticShader->program();
    causticUniforms.trailTex    = glGetUniformLocation(program, "trailTex");
    causticUniforms.velTexG     = glGetUniformLocation(program, "velTexG");
    causticUniforms.waveSubFrac = glGetUniformLocation(program, "waveSubFrac");
    causticUniforms.waveBias    = glGetUniformLocation(program, "waveBias");
    causticUniforms.gridN       = glGetUniformLocation(program, "gridN");
    causticUniforms.flowShift   = glGetUniformLocation(program, "flowShift");
    causticUniforms.causticK    = glGetUniformLocation(program, "causticK");
    return true;
}

bool CShaderManager::compileFluidShaders() {
    SP<CShader>* shaders[] = {&fluidAdvectShader, &fluidDivergenceShader,
                              &fluidJacobiShader, &fluidGradientShader};
    const char*  files[]   = {"fluid_advect.frag", "fluid_divergence.frag",
                              "fluid_jacobi.frag", "fluid_gradient.frag"};
    for (size_t i = 0; i < std::size(files); i++) {
        if (!(*shaders[i])->createProgram(
                g_pHyprOpenGL->m_shaders->TEXVERTSRC,
                loadShaderSource(files[i]),
                true
            )) {
            HyprlandAPI::addNotification(PHANDLE,
                std::format("[{}] Failed to compile {}", PLUGIN_NAME, files[i]),
                CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
            return false;
        }
    }

    auto program = fluidAdvectShader->program();
    fluidUniforms.aDt          = glGetUniformLocation(program, "dt");
    fluidUniforms.aDissipation = glGetUniformLocation(program, "dissipation");
    fluidUniforms.aForce       = glGetUniformLocation(program, "force");
    fluidUniforms.aForceDir    = glGetUniformLocation(program, "forceDir");

    program = fluidDivergenceShader->program();
    fluidUniforms.dTexelSize   = glGetUniformLocation(program, "texelSize");

    program = fluidJacobiShader->program();
    fluidUniforms.jTexelSize   = glGetUniformLocation(program, "texelSize");
    fluidUniforms.jDivTex      = glGetUniformLocation(program, "divTex");

    program = fluidGradientShader->program();
    fluidUniforms.gTexelSize   = glGetUniformLocation(program, "texelSize");
    fluidUniforms.gPrsTex      = glGetUniformLocation(program, "prsTex");

    return true;
}

void CShaderManager::initializeIfNeeded() {
    if (m_initialized)
        return;

    if (!compileGlassShader())
        return;

    if (!compileBlurShader())
        return;

    if (!compileAdaptiveLumaShader())
        return;

    if (!compileWaveSimShader())
        return;

    if (!compileFluidShaders())
        return;

    if (!compileTrailShader())
        return;

    if (!compileCausticShader())
        return;

    m_initialized = true;
}

void CShaderManager::destroy() noexcept {
    glassShader->destroy();
    blurShader->destroy();
    waveSimShader->destroy();
    trailShader->destroy();
    causticShader->destroy();
    fluidAdvectShader->destroy();
    fluidDivergenceShader->destroy();
    fluidJacobiShader->destroy();
    fluidGradientShader->destroy();
    m_initialized = false;
}
