# Architecture

## Layers

```
frontends
  dnr-video (CLI)      VapourSynth plugin (mpv)      [C ABI consumers]
        │                       │
        └────────── libdnr (C ABI) ──────────┘
                         │
             DLSSNR-AMD nr::Runtime (Vulkan compute)
             + SPIR-V shaders + dlssnr(-small).bin
```

`libdnr` owns the Vulkan device, image/staging resources and the `nr::Runtime`
instance. Frontends only pass RGBA8 frames.

## The runtime contract

`DLSSNR-AMD`'s `linux/src/core/nr_runtime.hpp` exposes:

```cpp
nr::Runtime rt(host, cfg);            // host = VkInstance/PhysicalDevice/Device/Queue
nr::ColourFrame frame{...};           // caller's VkImage, RGBA8 or RGBA32F
rt.record(cmd, frame, controls);      // single frame, no history
rt.record_temporal(cmd, frame, controls, {reset}); // history + fallback motion
```

`record()` only records; the caller submits and waits. This is what makes it
usable outside a game: no Present hook, no D3D interop, no ReShade.

`RuntimeConfig` essentials:

```cpp
cfg.root = "/path/to/dlssnr-root";  // contains dlssnr-amd/
cfg.width = w; cfg.height = h;
cfg.colour_format = VK_FORMAT_R8G8B8A8_UNORM;
cfg.linear_input = false;           // SDR sRGB-encoded input
cfg.model_scale = 1.0f;
cfg.max_passes = 1;
```

## Frame path

```
RGBA8 (CPU, sRGB-encoded)
  → persistent HOST_CACHED staging buffer
  → vkCmdCopyBufferToImage → VkImage (R8G8B8A8_UNORM, GENERAL)
  → nr::Runtime::record[_temporal] (records into one command buffer)
  → submit + fence
  → vkCmdCopyImageToBuffer → staging (invalidate)
  → RGBA8 (CPU)
```

Two details matter for performance:

- **8-bit frames** instead of RGBA32F: 4× less PCIe traffic.
- **`HOST_CACHED` staging**: `HOST_VISIBLE|HOST_COHERENT` BAR is uncached on this
  dGPU; reading it costs ~200 MB/s (8 MB ≈ 40 ms). Cached staging is ~2 ms.

## VapourSynth plugin

- API 4 (`VapourSynthPluginInit2`), namespace `dnr`, function `Enhance`.
- Input `RGB24` or `RGBS`; output is `RGB24`.
- Lazy init on the first frame; a global mutex serialises the single Vulkan runtime.
- On failure the original frame is returned and an error is logged; after repeated
  failures the filter becomes pass-through instead of breaking playback.
- Temporal state: `fmParallelRequests` with `arAllFramesReady` serial; non-consecutive
  frame numbers trigger `dnr_reset_history()`.

`dnr.vpy` does the colour plumbing:

```
mpv YUV (usually 4:2:0) → resize to RGB24 (matrix_in explicit)
  → core.dnr.Enhance(...)
  → resize back to YUV (default YUV444P8; mpv's VS bridge only accepts YUV)
```

mpv 0.41 does **not** dlopen `libvsscript.so`; it links `getVSScriptAPI()`.
The library must be visible through ld.so (see [BUILD](BUILD.md)); a symlink is not
enough because vsscript resolves its config via `dladdr()`.

## Model selection

The runtime looks for `dlssnr-small.bin` in the root:

- present → **small (310.9.16)** is the default;
- absent → `dlssnr.bin` (310.8.0 / 310.9.16 original preset);
- `DLSSNR_MODEL=original` forces the original; `DLSSNR_MODEL=small` forces small.

Small is ~40% faster (17.2 → 10.2 ms at 1080p) with the same output family
(PSNR 50.8 dB against the 310.9.16 reference).

## Why mpv and not VLC

VLC 3.0.x has no Vulkan vout and no way to hand a `VkDevice` to a video filter;
VLC 4's filter device enum still has no Vulkan. A VLC filter would add two
`sws_scale` conversions and run synchronously on the vout thread. VLC's own
packaging also forces `libvlccore-dev` and `VLC_PLUGIN_PATH`. Details:
[VLC.md](VLC.md).

## Threading / lifetime

- One context, one queue; `process` is synchronous.
- `dnr_destroy` waits for the device idle.
- The runtime writes `dlssnr-amd/pipeline.cache` on first build; keep the root
  writable.
- Environment: `NR_GPU` selects a physical device index. The host auto-selects the
  first discrete AMD device that reports both required extensions (the default
  enumeration order puts the iGPU first).
