#pragma once

#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

inline constexpr std::string_view CONFIG_PREFIX = "plugin:hyprwater:";

// Window tags for theme and preset selection
inline constexpr std::string_view TAG_THEME_PREFIX  = "hyprwater_theme_";
inline constexpr std::string_view TAG_PRESET_PREFIX = "hyprwater_preset_";

// Window tags for per-window enable/disable. Override the global `enabled` setting.
// `hyprwater_disabled` always wins if both are present.
inline constexpr std::string_view TAG_ENABLED  = "hyprwater_enabled";
inline constexpr std::string_view TAG_DISABLED = "hyprwater_disabled";

// Sentinel: "not set by user, inherit from parent layer"
inline constexpr Hyprlang::FLOAT SENTINEL_FLOAT = -1.0;
inline constexpr Hyprlang::INT   SENTINEL_INT   = -1;

inline constexpr int MAX_PRESET_INHERITANCE_DEPTH = 8;

namespace ConfigKeys {

// Global-only
inline constexpr auto ENABLED        = "plugin:hyprwater:enabled";
inline constexpr auto DEFAULT_THEME  = "plugin:hyprwater:default_theme";
inline constexpr auto DEFAULT_PRESET = "plugin:hyprwater:default_preset";

// Preset keyword, registered as unscoped because Hyprlang does not dispatch
// scoped keyword handlers inside the plugin special category.
inline constexpr auto PRESET_KEYWORD = "preset";

// Overridable — global level
inline constexpr auto BLUR_STRENGTH        = "plugin:hyprwater:blur_strength";
inline constexpr auto BLUR_ITERATIONS      = "plugin:hyprwater:blur_iterations";
inline constexpr auto REFRACTION_STRENGTH  = "plugin:hyprwater:refraction_strength";
inline constexpr auto CHROMATIC_ABERRATION = "plugin:hyprwater:chromatic_aberration";
inline constexpr auto FRESNEL_STRENGTH     = "plugin:hyprwater:fresnel_strength";
inline constexpr auto SPECULAR_STRENGTH    = "plugin:hyprwater:specular_strength";
inline constexpr auto GLASS_OPACITY        = "plugin:hyprwater:glass_opacity";
inline constexpr auto EDGE_THICKNESS       = "plugin:hyprwater:edge_thickness";
inline constexpr auto TINT_COLOR           = "plugin:hyprwater:tint_color";
// Per-pixel adaptive tint: where the backdrop showing through the glass is
// bright, tint just enough to bring it down to `adaptive_target` luma; where it
// is already dark, no tint at all. 0 disables (the static tint_alpha still
// applies). This is the passive/live replacement for polling the screen.
inline constexpr auto ADAPTIVE_TINT          = "plugin:hyprwater:adaptive_tint";
inline constexpr auto ADAPTIVE_TARGET        = "plugin:hyprwater:adaptive_target";
// Restrict adaptive tint to terminal windows (by WM class). 1 = terminals
// only, 0 = all windows (default, prior behaviour).
inline constexpr auto ADAPTIVE_TINT_TERMINALS_ONLY = "plugin:hyprwater:adaptive_tint_terminals_only";
// Seconds for the dim to settle. Animated wallpapers move the window average
// every frame; without this the dim pumps with the animation. ~2s reads as the
// eye adjusting rather than the picture flickering.
inline constexpr auto ADAPTIVE_SPEED         = "plugin:hyprwater:adaptive_speed";
inline constexpr auto LENS_DISTORTION      = "plugin:hyprwater:lens_distortion";
inline constexpr auto BRIGHTNESS           = "plugin:hyprwater:brightness";
inline constexpr auto CONTRAST             = "plugin:hyprwater:contrast";
inline constexpr auto SATURATION           = "plugin:hyprwater:saturation";
inline constexpr auto VIBRANCY             = "plugin:hyprwater:vibrancy";
inline constexpr auto VIBRANCY_DARKNESS    = "plugin:hyprwater:vibrancy_darkness";
inline constexpr auto ADAPTIVE_DIM          = "plugin:hyprwater:adaptive_dim";
inline constexpr auto ADAPTIVE_BOOST        = "plugin:hyprwater:adaptive_boost";

// Layer surface support
inline constexpr auto LAYERS_ENABLED            = "plugin:hyprwater:layers:enabled";
inline constexpr auto SHIMMER_ENABLED           = "plugin:hyprwater:shimmer:enabled";
inline constexpr auto SHIMMER_INTENSITY         = "plugin:hyprwater:shimmer:intensity";
inline constexpr auto SHIMMER_SPEED             = "plugin:hyprwater:shimmer:speed";
inline constexpr auto SHIMMER_SCALE             = "plugin:hyprwater:shimmer:scale";
inline constexpr auto SHIMMER_LIGHT_BACKDROP    = "plugin:hyprwater:shimmer:light_from_backdrop";
inline constexpr auto SHIMMER_DEPTH             = "plugin:hyprwater:shimmer:depth";
inline constexpr auto SHIMMER_AGITATION         = "plugin:hyprwater:shimmer:agitation";
inline constexpr auto SHIMMER_VISCOSITY         = "plugin:hyprwater:shimmer:viscosity";
inline constexpr auto SHIMMER_MURK              = "plugin:hyprwater:shimmer:murk";
inline constexpr auto SHIMMER_WINDOW_PHYSICS    = "plugin:hyprwater:shimmer:window_physics";
inline constexpr auto SHIMMER_MOUSE             = "plugin:hyprwater:shimmer:mouse";
inline constexpr auto SHIMMER_CURRENTS          = "plugin:hyprwater:shimmer:currents";
inline constexpr auto SHIMMER_CURRENTS_RES      = "plugin:hyprwater:shimmer:currents_resolution";
inline constexpr auto SHIMMER_ABSORPTION        = "plugin:hyprwater:shimmer:absorption";
inline constexpr auto SHIMMER_BED               = "plugin:hyprwater:shimmer:bed_variation";
inline constexpr auto LAYERS_NAMESPACES         = "plugin:hyprwater:layers:namespaces";
inline constexpr auto LAYERS_EXCLUDE_NAMESPACES = "plugin:hyprwater:layers:exclude_namespaces";
inline constexpr auto LAYERS_PRESET             = "plugin:hyprwater:layers:preset";
inline constexpr auto LAYERS_NAMESPACE_PRESETS          = "plugin:hyprwater:layers:namespace_presets";
inline constexpr auto LAYERS_NAMESPACE_MASK_THRESHOLDS  = "plugin:hyprwater:layers:namespace_mask_thresholds";

// Overridable — dark theme overrides
inline constexpr auto DARK_BLUR_STRENGTH        = "plugin:hyprwater:dark:blur_strength";
inline constexpr auto DARK_BLUR_ITERATIONS      = "plugin:hyprwater:dark:blur_iterations";
inline constexpr auto DARK_REFRACTION_STRENGTH  = "plugin:hyprwater:dark:refraction_strength";
inline constexpr auto DARK_CHROMATIC_ABERRATION = "plugin:hyprwater:dark:chromatic_aberration";
inline constexpr auto DARK_FRESNEL_STRENGTH     = "plugin:hyprwater:dark:fresnel_strength";
inline constexpr auto DARK_SPECULAR_STRENGTH    = "plugin:hyprwater:dark:specular_strength";
inline constexpr auto DARK_GLASS_OPACITY        = "plugin:hyprwater:dark:glass_opacity";
inline constexpr auto DARK_EDGE_THICKNESS       = "plugin:hyprwater:dark:edge_thickness";
inline constexpr auto DARK_TINT_COLOR           = "plugin:hyprwater:dark:tint_color";
inline constexpr auto DARK_LENS_DISTORTION      = "plugin:hyprwater:dark:lens_distortion";
inline constexpr auto DARK_BRIGHTNESS           = "plugin:hyprwater:dark:brightness";
inline constexpr auto DARK_CONTRAST             = "plugin:hyprwater:dark:contrast";
inline constexpr auto DARK_SATURATION           = "plugin:hyprwater:dark:saturation";
inline constexpr auto DARK_VIBRANCY             = "plugin:hyprwater:dark:vibrancy";
inline constexpr auto DARK_VIBRANCY_DARKNESS    = "plugin:hyprwater:dark:vibrancy_darkness";
inline constexpr auto DARK_ADAPTIVE_DIM          = "plugin:hyprwater:dark:adaptive_dim";
inline constexpr auto DARK_ADAPTIVE_BOOST        = "plugin:hyprwater:dark:adaptive_boost";

// Overridable — light theme overrides
inline constexpr auto LIGHT_BLUR_STRENGTH        = "plugin:hyprwater:light:blur_strength";
inline constexpr auto LIGHT_BLUR_ITERATIONS      = "plugin:hyprwater:light:blur_iterations";
inline constexpr auto LIGHT_REFRACTION_STRENGTH  = "plugin:hyprwater:light:refraction_strength";
inline constexpr auto LIGHT_CHROMATIC_ABERRATION = "plugin:hyprwater:light:chromatic_aberration";
inline constexpr auto LIGHT_FRESNEL_STRENGTH     = "plugin:hyprwater:light:fresnel_strength";
inline constexpr auto LIGHT_SPECULAR_STRENGTH    = "plugin:hyprwater:light:specular_strength";
inline constexpr auto LIGHT_GLASS_OPACITY        = "plugin:hyprwater:light:glass_opacity";
inline constexpr auto LIGHT_EDGE_THICKNESS       = "plugin:hyprwater:light:edge_thickness";
inline constexpr auto LIGHT_TINT_COLOR           = "plugin:hyprwater:light:tint_color";
inline constexpr auto LIGHT_LENS_DISTORTION      = "plugin:hyprwater:light:lens_distortion";
inline constexpr auto LIGHT_BRIGHTNESS           = "plugin:hyprwater:light:brightness";
inline constexpr auto LIGHT_CONTRAST             = "plugin:hyprwater:light:contrast";
inline constexpr auto LIGHT_SATURATION           = "plugin:hyprwater:light:saturation";
inline constexpr auto LIGHT_VIBRANCY             = "plugin:hyprwater:light:vibrancy";
inline constexpr auto LIGHT_VIBRANCY_DARKNESS    = "plugin:hyprwater:light:vibrancy_darkness";
inline constexpr auto LIGHT_ADAPTIVE_DIM          = "plugin:hyprwater:light:adaptive_dim";
inline constexpr auto LIGHT_ADAPTIVE_BOOST        = "plugin:hyprwater:light:adaptive_boost";

} // namespace ConfigKeys

// Built-in terminal WM classes for the adaptive_tint_terminals_only restriction.
inline bool isTerminalClass(const std::string& cls) {
    static const std::unordered_set<std::string> terminals = {
        "kitty", "alacritty", "wezterm", "foot", "konsole",
        "xfce4-terminal", "gnome-terminal-server", "xterm", "st",
        "urxvt", "terminator", "warp", "qterminal", "tilix"
    };
    return terminals.count(cls) != 0;
}

// Cached pointers for a single config layer (built-in dark/light/global)
struct SOverridableConfig {
    Hyprlang::FLOAT* const* blurStrength        = nullptr;
    Hyprlang::INT* const*   blurIterations      = nullptr;
    Hyprlang::FLOAT* const* refractionStrength  = nullptr;
    Hyprlang::FLOAT* const* chromaticAberration = nullptr;
    Hyprlang::FLOAT* const* fresnelStrength     = nullptr;
    Hyprlang::FLOAT* const* specularStrength    = nullptr;
    Hyprlang::FLOAT* const* glassOpacity        = nullptr;
    Hyprlang::FLOAT* const* edgeThickness       = nullptr;
    Hyprlang::INT* const*   tintColor           = nullptr;
    Hyprlang::FLOAT* const* lensDistortion      = nullptr;
    Hyprlang::FLOAT* const* brightness          = nullptr;
    Hyprlang::FLOAT* const* contrast            = nullptr;
    Hyprlang::FLOAT* const* saturation          = nullptr;
    Hyprlang::FLOAT* const* vibrancy            = nullptr;
    Hyprlang::FLOAT* const* vibrancyDarkness    = nullptr;
    Hyprlang::FLOAT* const* adaptiveDim         = nullptr;
    Hyprlang::FLOAT* const* adaptiveBoost       = nullptr;
};

// Plain values for a user-defined preset layer (all sentinel = not set → inherit)
struct SPresetValues {
    float   blurStrength       = static_cast<float>(SENTINEL_FLOAT);
    int64_t blurIterations     = SENTINEL_INT;
    float   refractionStrength = static_cast<float>(SENTINEL_FLOAT);
    float   chromaticAberration = static_cast<float>(SENTINEL_FLOAT);
    float   fresnelStrength    = static_cast<float>(SENTINEL_FLOAT);
    float   specularStrength   = static_cast<float>(SENTINEL_FLOAT);
    float   glassOpacity       = static_cast<float>(SENTINEL_FLOAT);
    float   edgeThickness      = static_cast<float>(SENTINEL_FLOAT);
    int64_t tintColor          = SENTINEL_INT;
    float   lensDistortion     = static_cast<float>(SENTINEL_FLOAT);
    float   brightness         = static_cast<float>(SENTINEL_FLOAT);
    float   contrast           = static_cast<float>(SENTINEL_FLOAT);
    float   saturation         = static_cast<float>(SENTINEL_FLOAT);
    float   vibrancy           = static_cast<float>(SENTINEL_FLOAT);
    float   vibrancyDarkness   = static_cast<float>(SENTINEL_FLOAT);
    float   adaptiveDim        = static_cast<float>(SENTINEL_FLOAT);
    float   adaptiveBoost      = static_cast<float>(SENTINEL_FLOAT);
};

struct SCustomPreset {
    std::string   name;
    std::string   inherits;
    SPresetValues shared;
    SPresetValues dark;
    SPresetValues light;
};

struct StringConfigPtr {
    void* const*          dataptr = nullptr;
    const std::type_info* type    = nullptr;
};

inline std::string_view readStringConfig(const StringConfigPtr& ptr) {
    if (!ptr.dataptr || !ptr.type)
        return {};

    if (*ptr.type == typeid(Config::STRING)) {
        const auto* value = *reinterpret_cast<Config::STRING* const*>(ptr.dataptr);
        return value ? std::string_view(*value) : std::string_view{};
    }

    if (*ptr.type == typeid(Hyprlang::STRING)) {
        const auto value = *reinterpret_cast<Hyprlang::STRING const*>(ptr.dataptr);
        return value ? std::string_view(value) : std::string_view{};
    }

    return {};
}

struct SPluginConfig {
    Hyprlang::INT* const* enabled       = nullptr;
    StringConfigPtr      defaultTheme;
    StringConfigPtr      defaultPreset;

    Hyprlang::INT* const* layersEnabled                  = nullptr;
    Hyprlang::INT* const*   shimmerEnabled              = nullptr;
    Hyprlang::FLOAT* const* adaptiveTint                = nullptr;
    Hyprlang::FLOAT* const* adaptiveTarget              = nullptr;
    Hyprlang::FLOAT* const* adaptiveSpeed               = nullptr;
    Hyprlang::INT* const*   adaptiveTintTerminalsOnly   = nullptr;
    Hyprlang::FLOAT* const* shimmerIntensity            = nullptr;
    Hyprlang::FLOAT* const* shimmerSpeed                = nullptr;
    Hyprlang::FLOAT* const* shimmerScale                = nullptr;
    Hyprlang::INT* const*   shimmerLightFromBackdrop   = nullptr;
    Hyprlang::FLOAT* const* shimmerDepth               = nullptr;
    Hyprlang::FLOAT* const* shimmerAgitation           = nullptr;
    Hyprlang::FLOAT* const* shimmerViscosity           = nullptr;
    Hyprlang::FLOAT* const* shimmerMurk                = nullptr;
    Hyprlang::FLOAT* const* shimmerWindowPhysics       = nullptr;
    Hyprlang::FLOAT* const* shimmerMouse               = nullptr;
    Hyprlang::INT* const*   shimmerCurrents            = nullptr;
    Hyprlang::INT* const*   shimmerCurrentsRes         = nullptr;
    Hyprlang::FLOAT* const* shimmerAbsorption          = nullptr;
    Hyprlang::FLOAT* const* shimmerBed                 = nullptr;
    StringConfigPtr       layersNamespaces;
    StringConfigPtr       layersExcludeNamespaces;
    StringConfigPtr       layersPreset;
    StringConfigPtr       layersNamespacePresets;
    StringConfigPtr       layersNamespaceMaskThresholds;

    SOverridableConfig global;
    SOverridableConfig dark;
    SOverridableConfig light;
};

// Context for preset-aware value resolution
struct SResolveContext {
    const std::string&                                    presetName;
    bool                                                  isDark;
    const SPluginConfig&                                  config;
    const std::unordered_map<std::string, SCustomPreset>& customPresets;
};

// Preset-aware resolution: preset chain → built-in theme → global → hardcoded
[[nodiscard]] float resolvePresetFloat(
    const SResolveContext& context,
    float SPresetValues::* presetField,
    Hyprlang::FLOAT* const* SOverridableConfig::* configField,
    float hardcodedDefault = static_cast<float>(SENTINEL_FLOAT));

[[nodiscard]] int64_t resolvePresetInt(
    const SResolveContext& context,
    int64_t SPresetValues::* presetField,
    Hyprlang::INT* const* SOverridableConfig::* configField,
    int64_t hardcodedDefault = SENTINEL_INT);

void registerConfig(HANDLE handle);
void initConfigPointers(HANDLE handle, SPluginConfig& config);

// Preset keyword handler (registered via addConfigKeyword)
Hyprlang::CParseResult handlePresetKeyword(const char* command, const char* value);

// Clear pending presets/layers before config re-parse (called from preConfigReload callback)
void clearPendingPresets();
void clearPendingLayers();

// Swap pending data into active maps (called from configReloaded callback)
void commitPendingPresets();
void commitPendingLayers();

// Validate config values and notify user of misconfigurations
void validateConfig();
