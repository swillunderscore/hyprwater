# hyprwater

Water for Hyprland windows. A real-time wave simulation rendered as liquid
glass: windows are viewports onto one shared sheet of water. Waves propagate,
reflect, and interfere; light refracts through the surface and lands on
whatever is behind the window.

Started as a fork of [HyprGlass](https://github.com/hyprnux/hyprglass) by
Hyprnux — the blur/refraction glass pipeline remains from there. The water is
new: the simulation, the caustics, the fluid dynamics, and the physics that
ties them together.

## What it does

- **Wave simulation, not animated noise.** A GPU wave-equation integrator:
  disturbances spread at finite speed, reflect off edges, interfere with their
  own reflections, and settle when the water is left alone.
- **Energy-conserving caustics.** Light is transported to where refraction
  actually sends it (forward splatting). Bright veins are paid for by the
  water around them going dim, and a finite-size sun blurs the pattern with
  depth, the way real pools work.
- **Currents.** A Stable Fluids velocity field: dragging a window leaves real
  whirlpools that carry and bend the waves crossing them.
- **Window physics.** Moving windows push a bow wave and shed a wake. Clicks
  splash. The cursor leaves a wake. All of it feeds one shared sheet, so
  overlapping windows agree about the water.
- **One physical depth.** The depth setting is in meters and drives four
  things at once: refraction (Snell), caustic focus, wave speed, and
  Beer–Lambert absorption (deep water turns blue because red is absorbed —
  the real coefficients, Pope & Fry 1997).
- **Layer surfaces too.** Bars, notification popups, and other layer-shell
  surfaces can be glassed, with per-namespace presets and mask thresholds.

## Requirements

| | |
|---|---|
| Hyprland | 0.56.x |
| Build | `g++` (C++23), `pkg-config`, hyprland headers, pixman, libdrm |

## Install

With hyprpm:

```sh
hyprpm add https://github.com/swillunderscore/hyprwater
hyprpm enable hyprwater
```

Manual:

```sh
make
hyprctl plugin load "$(pwd)/hyprwater.so"
```

## Configuration

Everything lives under `plugin:hyprwater:`. The water:

| key | default | what it is |
|---|---|---|
| `shimmer:enabled` | 1 | the water on/off |
| `shimmer:intensity` | — | caustic light strength (exposure-like) |
| `shimmer:speed` | 1.0 | simulation rate; below 1 is slow motion |
| `shimmer:scale` | — | wave size on screen |
| `shimmer:depth` | 1.0 | water depth in meters; drives refraction, caustic focus, wave speed, and absorption |
| `shimmer:viscosity` | 0.6 | thicker water kills short ripples faster (real ν·k² dissipation) |
| `shimmer:agitation` | — | how often the ambient pool is disturbed |
| `shimmer:currents` | 1 | the velocity field (whirlpools) |
| `shimmer:currents_resolution` | 512 | fluid grid resolution |
| `shimmer:window_physics` | 1.0 | bow waves and wakes from moving windows |
| `shimmer:mouse` | 0.3 | cursor wake and click splashes; 0 = hands off the water |
| `shimmer:murk` | 0 | scattering: haze that veils the backdrop (absorption is always on and physical) |
| `shimmer:light_from_backdrop` | 1 | caustics are lit by what is behind the window rather than a lamp |

The glass (from HyprGlass, with `dark:`/`light:` theme variants for each):
`blur_strength`, `blur_iterations`, `refraction_strength`, `fresnel_strength`,
`specular_strength`, `chromatic_aberration`, `edge_thickness`, `glass_opacity`,
`tint_color`, `lens_distortion`, `saturation`, `brightness`, `contrast`,
`vibrancy`, `vibrancy_darkness`, `adaptive_dim`, `adaptive_boost`.

Layer surfaces: `layers:enabled`, `layers:namespaces`,
`layers:exclude_namespaces`, `layers:namespace_presets`,
`layers:namespace_mask_thresholds`, `layers:preset`.

Presets: named bundles of the glass settings, defined in config and applied
per window rule (`tag:hyprwater_preset_<name>`) or per layer namespace.
`default_theme` / `tag:hyprwater_theme_light` pick the theme variant.

<details>
<summary>All keys</summary>

- `plugin:hyprwater:adaptive_boost`
- `plugin:hyprwater:adaptive_dim`
- `plugin:hyprwater:blur_iterations`
- `plugin:hyprwater:blur_strength`
- `plugin:hyprwater:brightness`
- `plugin:hyprwater:chromatic_aberration`
- `plugin:hyprwater:contrast`
- `plugin:hyprwater:default_preset`
- `plugin:hyprwater:default_theme`
- `plugin:hyprwater:edge_thickness`
- `plugin:hyprwater:enabled`
- `plugin:hyprwater:fresnel_strength`
- `plugin:hyprwater:glass_opacity`
- `plugin:hyprwater:layers:enabled`
- `plugin:hyprwater:layers:exclude_namespaces`
- `plugin:hyprwater:layers:namespace_mask_thresholds`
- `plugin:hyprwater:layers:namespace_presets`
- `plugin:hyprwater:layers:namespaces`
- `plugin:hyprwater:layers:preset`
- `plugin:hyprwater:lens_distortion`
- `plugin:hyprwater:refraction_strength`
- `plugin:hyprwater:saturation`
- `plugin:hyprwater:shimmer:absorption`
- `plugin:hyprwater:shimmer:agitation`
- `plugin:hyprwater:shimmer:bed_variation`
- `plugin:hyprwater:shimmer:currents`
- `plugin:hyprwater:shimmer:currents_resolution`
- `plugin:hyprwater:shimmer:depth`
- `plugin:hyprwater:shimmer:enabled`
- `plugin:hyprwater:shimmer:intensity`
- `plugin:hyprwater:shimmer:light_from_backdrop`
- `plugin:hyprwater:shimmer:mouse`
- `plugin:hyprwater:shimmer:murk`
- `plugin:hyprwater:shimmer:scale`
- `plugin:hyprwater:shimmer:speed`
- `plugin:hyprwater:shimmer:viscosity`
- `plugin:hyprwater:shimmer:window_physics`
- `plugin:hyprwater:specular_strength`
- `plugin:hyprwater:tint_color`
- `plugin:hyprwater:vibrancy`
- `plugin:hyprwater:vibrancy_darkness`

</details>

## Notes

- The simulation runs on its own framebuffers (fp16). Cost on a desktop GPU
  is a few percent; the caustic pass is rate-limited and resolution-bound.
- The wave equation is linear: waves do not break. Ambient chop is
  self-limiting instead.
- Plugins must be rebuilt when Hyprland updates. hyprpm handles this; for
  manual builds, rebuild before loading into a new Hyprland version.

## Credits

Fork of [HyprGlass](https://github.com/hyprnux/hyprglass) by Hyprnux —
the glass rendering pipeline is theirs. Everything water is
[Swill Software](https://github.com/swillunderscore).

## License

BSD-3-Clause. The LICENSE file carries both copyright lines.
