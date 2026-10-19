# dlss-nr-player

**DLSS 5 Neural Rendering for video playback on AMD Linux.**

A native Vulkan host for [DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)'s
`nr::Runtime`, exposed as:

- **`dnr-video`** — a rawvideo CLI (decode → NR → encode), useful for offline conversion and benchmarking;
- **`dnr` VapourSynth 4 plugin** — real-time NR inside **mpv** (`--vf=vapoursynth`);
- a small **C ABI (`libdnr.so`)** that other players/filters can link.

It runs the DLSS 5 NR network (the same network as NVIDIA's `nvngx_dlssnr.dll`)
on RDNA4 through Vulkan compute, with the weights extracted from a DLL **you own**.
No NVIDIA binaries or weights are included in this repository.

> Tested on **AMD RX 9060 XT (RADV GFX1200)**, Mesa 25.3.3, 16-core Ubuntu 25.04 container.
> 1080p: **39.6 fps in mpv**, **57.8 fps in the CLI**, PSNR **50.80 dB** against the
> 310.9.16 reference output. 4K is not real-time (8.16 fps), see [performance](docs/PERFORMANCE.md).

---

## How it works

```
FFmpeg decode ──► RGBA8 (CPU) ──► VkImage ──► nr::Runtime (Vulkan compute)
                                                   │
mpv --vf=vapoursynth ◄── VapourSynth plugin ◄──────┘
                                                   │
                                        readback ──┴──► encode / display
```

- The runtime records into the caller's `VkCommandBuffer` on the caller's `VkDevice`.
- The plugin keeps a **session-level** device/runtime; only the first frame pays the
  weight upload (~0.6 s with a warm pipeline cache, ~20 s cold).
- Colour: SDR, sRGB-encoded 0..1 input, `linear_input=false`; the NR network is not an
  upscaler (same resolution in/out).

## Requirements

| Item | Requirement |
|---|---|
| GPU | AMD RDNA4 (RX 9000 / gfx1200/gfx1201). RDNA2/3 have no FP8 WMMA path in this runtime. |
| Driver | Mesa **25.3.3+** with `VK_KHR_cooperative_matrix` and `VK_EXT_shader_float8` (or the AMD Windows driver with the same extensions). |
| DLSSNR-AMD | Source checkout (MIT). The runtime/shaders are **not vendored** here. |
| Model | `nvngx_dlssnr.dll` **310.8.0 or 310.9.16** that you legally own; extract with DLSSNR-AMD's `model-tools/extract_model.sh`. |
| Build | g++ (C++17), Vulkan headers ≥ 1.4.304, `libvulkan`; CMake ≥ 3.16 for the plugin. |
| Player | mpv built with `-Dvapoursynth=enabled`, VapourSynth R81 (API 4). Ubuntu 25.04 has neither packaged; `scripts/build-vapoursynth-mpv.sh` builds both. |

## Quick start

### 1. Get DLSSNR-AMD and prepare the model

```bash
git clone https://github.com/mochizuki0323/DLSSNR-AMD.git /opt/DLSSNR-AMD

# Put your own nvngx_dlssnr.dll (310.8.0 or 310.9.16) somewhere, then:
mkdir -p /opt/dlssnr-root/dlssnr-amd
# The installer / extractor in DLSSNR-AMD creates:
#   dlssnr.bin          (310.8.0, or the 310.9.16 "original" preset)
#   dlssnr-small.bin    (310.9.16 small/new model, optional but recommended)
# and copies the shader tree next to them.
```

The runtime only needs this layout:

```
/opt/dlssnr-root/
└── dlssnr-amd/
    ├── dlssnr.bin
    ├── dlssnr-small.bin   # optional; when present it is the default
    └── shaders/           # from the DLSSNR-AMD package
```

### 2. Build the core and the plugin

```bash
git clone https://github.com/lj189cn/dlss-nr-player.git
cd dlss-nr-player

DLSSNR_AMD_SRC=/opt/DLSSNR-AMD scripts/build-core.sh
# → build/dnr-video, build/libdnr.so

DNR_LIBRARY=$PWD/build/libdnr.so scripts/build-vs-plugin.sh --prefix /usr/local
# → /usr/local/lib/vapoursynth/dnr.so
# The plugin has RUNPATH=$ORIGIN; copy libdnr.so next to it if you do not install it:
#   cp build/libdnr.so /usr/local/lib/vapoursynth/
```

### 3. Build VapourSynth + mpv (Ubuntu/Debian)

Ubuntu/Debian do not ship VapourSynth, and their mpv is built without
`--enable-vapoursynth`. Build both from source (≈2–3 min on 16 cores):

```bash
sudo scripts/build-vapoursynth-mpv.sh --prefix /usr/local
```

If your Mesa was installed with `--force-depends` (e.g. a custom 25.3.3 package),
run `scripts/fix-wayland.sh` first; see [docs/WOLFLUTRIS.md](docs/WOLFLUTRIS.md).

### 4. Play

```bash
export DNR_ROOT=/opt/dlssnr-root
export DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so
mpv --vo=gpu-next --gpu-api=vulkan \
    --vf=vapoursynth=$PWD/dnr.vpy movie.mkv
```

A/B and tuning:

```bash
DNR_BYPASS_FILE=/tmp/dnr-off mpv ...     # file exists = bypass NR
DNR_INTENSITY=0.5 mpv ...                # 0..2, default 1.0
DNR_MODEL_SCALE=0.75 mpv ...             # run the model at 75% for speed
DNR_TEMPORAL=0 mpv ...                   # disable history/motion path
DLSSNR_MODEL=original mpv ...            # force the old model (if small is installed)
```

Offline CLI:

```bash
ffmpeg -i in.mp4 -vf "scale=1920:1080,format=rgba" -f rawvideo -pix_fmt rgba - \
| build/dnr-video --width 1920 --height 1080 --root /opt/dlssnr-root \
| ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - -c:v libx264 -crf 18 out.mp4
```

See [docs/USAGE.md](docs/USAGE.md) for all options and the C API.

## Performance (RX 9060 XT, 1080p, 300 frames)

| Path | Model | Network | End-to-end | FPS |
|---|---|---|---|---|
| `dnr-video` | 310.9.16 small | 10.19 ms | 17.29 ms | **57.8** |
| mpv + plugin | 310.9.16 small | 10.32 ms | — | **39.6** |
| mpv + plugin | 310.8.0 original | 17.18 ms | — | 30.6 |
| mpv + plugin, scale 0.75 | small | 6.37 ms | — | 49.9 |
| 4K (3840×2160) mpv | small | 67.1 ms | 122.5 ms/frame | 8.16 |

1080p 10-minute run: 17 900 frames, **0 dropped frames**, VRAM constant at +1001 MiB,
no crash. 4K uses +2354 MiB and is not real-time.

## Repository layout

```
src/                 libdnr (C ABI), dnr-video CLI, VapourSynth plugin, stub
scripts/             build-core, build-vs-plugin, build-vapoursynth-mpv, fix-wayland,
                     run-mpv-test, m4-stability, test-model-3109, wolf/*
dnr.vpy              VapourSynth script used by mpv
third_party/         VapourSynth R81 API 4 headers (LGPL-2.1)
docs/                architecture, build, usage, performance, testing, VLC memo
```

## Status / limitations

- **Working**: 1080p real-time playback with the small model; A/B, intensity, scale;
  offline CLI; 10-minute stability; GPU display via mpv `gpu-next` + Vulkan.
- **Not real-time**: 4K (≈8 fps). CPU↔GPU copies and YUV↔RGB conversions cost ≈8–15 ms/frame.
- **Not implemented**: zero-copy (VAAPI dmabuf → Vulkan), frame pipelining (2–3 in flight),
  HDR tonemapping, subtitles-aware NR, Windows frontend.
- **VLC**: evaluated and **not** the primary path — VLC 3 has no Vulkan and runs video
  filters synchronously on the vout thread. See [docs/VLC.md](docs/VLC.md).

## License

MIT for this repository's code. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

DLSSNR-AMD is MIT; its runtime, shaders and the DLSS 5 network are **not** bundled here.
`nvngx_dlssnr.dll` and the extracted weights are NVIDIA property — use only copies you
are allowed to use, and do not redistribute them.

Not affiliated with NVIDIA or AMD.
