# framefov

VRS-based foveated rendering for **Steam Frame** (aarch64 SteamOS, Turnip/Adreno
750). A Vulkan layer that injects a fragment-shading-rate attachment into every
render pass and fills it with a foveated pattern.

Why it exists: Valve's `VALVE_fdm_injection` only foveates images it owns (the XR
swapchain). Native OpenVR titles on the Frame render into their own DXVK
textures, so Valve's layer has nothing to attach to: pipelines are created with
`set fdm flag: 0`, no pass gets a map, and per-view offsets are unavailable
(`offsets_disabled`), because the offset bit is only added to images Valve
created. This layer does not need any of that.

## Build

```sh
cmake -B build -GNinja -DFRAMEFOV_USE_SYSTEM_HEADERS=ON
cmake --build build
ctest --test-dir build
```

## Install

```sh
scripts/install-frame.sh build
```

Installs an implicit layer to `~/.local/lib/framefov/` and
`~/.local/share/vulkan/implicit_layer.d/`, gated on `FRAMEFOV_ENABLE=1`.

## Use

Turn **Foveated Rendering** off in the game's Steam settings — it enables Valve's
layer, which conflicts with this one.

Steam launch options for a native OpenVR title:

```
FRAMEFOV_ENABLE=1 FRAMEFOV_EYE_SIZE=2176x2160 %command%
```

With eye tracking:

```sh
systemctl --user enable --now framefov-gaze
```

```
FRAMEFOV_ENABLE=1 FRAMEFOV_OPENVR_GAZE=1 FRAMEFOV_EYE_SIZE=2176x2160 %command%
```

`framefov-gaze` reads the eye-tracked foveation centre over OpenVR and publishes
it to `/dev/shm/framefov-gaze`; the layer only reads that file.

### Environment

| variable | meaning |
|---|---|
| `FRAMEFOV_ENABLE` | required; the layer loads only when set |
| `FRAMEFOV_OPENVR_GAZE` | read gaze from the `framefov-gaze` publisher |
| `FRAMEFOV_OPENVR_CLIENT` | publisher's SteamVR client type: `2`/`other` (default), `3`/`overlay`, `5`/`utility` |
| `FRAMEFOV_EYE_SIZE` | expected per-eye size, `WxH`; targets are matched by aspect/size |
| `FRAMEFOV_SIZE_TOLERANCE` | aspect tolerance (default 0.02) |
| `FRAMEFOV_FIXED_INNER_RADIUS` / `_OUTER_RADIUS` | fixed-mode radii (default 0.286 / 0.95) |
| `FRAMEFOV_INNER_RADIUS` / `_OUTER_RADIUS` | eye-tracked radii (default 0.286 / 0.95) |
| `FRAMEFOV_MIDDLE_RATE` / `_OUTER_RATE` | e.g. `2x2`, `4x4` (defaults `2x2` / `2x2`) |
| `FRAMEFOV_HORIZONTAL_SCALE` | horizontal stretch of the pattern |
| `FRAMEFOV_SMOOTHING` | EMA on the gaze, 0 = none, 0.9 = heavy |
| `FRAMEFOV_PRESET` | `lo` (default), `med`, `hi` |
| `FRAMEFOV_GAZE_YFLIP` | mirror the gaze vertically |
| `FRAMEFOV_DEBUG_MODE` | 1 = outer rate everywhere, 2 = inverted (centre coarse) |
| `FRAMEFOV_DRAW_RADII` | 1 = rings at the radii, 2 = inner only, 3 = outer only |
| `FRAMEFOV_LOG_LEVEL` | `info` (default), `debug` |
| `FRAMEFOV_LOG_STDERR` | also log INFO/DEBUG to stderr |

Log file: `~/.local/state/framefov/framefov.log`.

### Presets

| preset | clear area (ellipse) | middle | outer | hscale |
|---|---|---|---|---|
| `lo` | `r=0.286` (.295 wide x .286 tall) | 2x2 | 2x2 out to `r=0.95` | 1.03 |
| `med` | `r=0.245` (.250 x .245) | 2x2 | 4x4 beyond `r=0.367` | 1.03 |
| `hi` | `r=0.211` (.246 x .211) | 2x2 | 4x4 beyond `r=0.351` | 1.12 |

Any explicit radius/rate variable overrides the preset.

## Known issues

- The upstream GPU test (`FRAMEFOV_BUILD_GPU_TESTS=ON`) is unreliable on Turnip
  and is off by default.
- Valve's `VK_LAYER_VALVE_fdm_injection` wins where it attaches a map; this layer
  then backs off. Disable it with `VK_LOADER_LAYERS_DISABLE` for clean
  comparisons.
- Turnip quantises the rate to GMEM bins, so the sharp area snaps to a bin grid.

## License

MIT — see [`LICENSE`](LICENSE). Third-party build dependencies are listed in
[`NOTICE`](NOTICE).
