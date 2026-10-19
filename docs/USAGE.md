# Usage

## mpv

```bash
export DNR_ROOT=/opt/dlssnr-root
export DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so

mpv --no-config --vo=gpu-next --gpu-api=vulkan \
    --vf=vapoursynth=/path/to/dlss-nr-player/dnr.vpy \
    /path/to/video.mkv
```

Headless / no display (still runs the filter):

```bash
mpv --no-config --no-audio --ao=null --vo=null --untimed --frames=300 \
    --vf=vapoursynth=$PWD/dnr.vpy sample.mp4
```

A/B switch and tuning (read per-frame or at startup):

| Variable | Default | Meaning |
|---|---|---|
| `DNR_ROOT` | — | directory containing `dlssnr-amd/` |
| `DNR_PLUGIN` | autodetect | full path to `dnr.so` (if not in VS autoload dir) |
| `DNR_INTENSITY` | `1.0` | 0..2, network output strength |
| `DNR_STYLE` | `0` | reserved model style |
| `DNR_TEMPORAL` | `1` | `0` disables history/motion (`record()` instead of `record_temporal()`) |
| `DNR_MODEL_SCALE` | `1.0` | run the model at a fraction of the frame (0,1], e.g. 0.75 |
| `DNR_MATRIX` | `1` (BT.709) | input/output matrix; SD use `5` (BT.601) |
| `DNR_MATRIX_IN` / `DNR_MATRIX_OUT` | `DNR_MATRIX` | override one side |
| `DNR_RANGE_IN` / `DNR_RANGE_OUT` | frame props | 0 limited / 1 full |
| `DNR_OUT_FORMAT` | `yuv444p8` | `yuv420p8` / `yuv422p8` also accepted |
| `DNR_BYPASS_FILE` | — | if this file exists, pass through (A/B) |
| `DNR_INTENSITY_FILE` | — | read a float from this file each frame |
| `DLSSNR_MODEL` | auto | `small` or `original` |
| `NR_GPU` | auto | Vulkan physical-device index |

Interactive A/B with `scripts/dnr-toggle.lua`:

```bash
DNR_BYPASS_FILE=/tmp/dnr-off DNR_INTENSITY_FILE=/tmp/dnr-intensity \
mpv --script=scripts/dnr-toggle.lua --vf=vapoursynth=$PWD/dnr.vpy movie.mkv
# n = toggle, [ / ] = intensity -/+ 0.1
```

## CLI

`dnr-video` reads/writes **raw RGBA8** on stdin/stdout (or `--in/--out`), so it
does not link FFmpeg. All logs go to stderr.

```bash
ffmpeg -i in.mp4 -vf "scale=1920:1080,format=rgba" -f rawvideo -pix_fmt rgba - \
| dnr-video --width 1920 --height 1080 --root /opt/dlssnr-root \
| ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - -c:v libx264 -crf 18 out.mp4
```

Options:

```
--in FILE|-        input raw RGBA8 (default -)
--out FILE|-       output raw RGBA8 (default -)
--width W --height H
--root DIR         DLSSNR root (contains dlssnr-amd/)
--frames N         0 = until EOF
--bench            measure only, no output
--intensity F      0..2 (default 1)
--style N
--temporal | --no-temporal   default temporal on
--reset-every N    forget history every N frames
--model-scale F    default 1.0
--gpu N            Vulkan device index
--quiet
```

## C API

`src/dnr_c.h` is a small, versioned ABI:

```c
#include "dnr_c.h"

dnr_config cfg = {
    .root = "/opt/dlssnr-root",
    .width = 1920, .height = 1080,
    .temporal = 1, .model_scale = 1.0f,
    .gpu_index = -1, .verbose = 1,
    .cfg_size = sizeof(cfg),
};
char err[256];
dnr_context *ctx = dnr_create(&cfg, err, sizeof err);
if (!ctx) { fprintf(stderr, "%s\n", err); return 1; }

dnr_controls c = { .intensity = 1.0f, .style = 0, .reset_history = 0,
                   .ctrl_size = sizeof(c) };
float net_ms = 0;
int rc = dnr_process_rgba(ctx, src, dst, &c, &net_ms);
dnr_destroy(ctx);
```

- RGBA8, tightly packed rows, `stride == width*4`.
- One context is not thread-safe; serialise calls.
- `dnr_reset_history()` on seek / frame discontinuity.
- `out_network_ms` is the network time only (the submit/fence/copy overhead is not included).

## Colour

- The model expects **SDR, sRGB-encoded 0..1** values. Do not feed linear light.
- HDR (PQ/HLG) must be tonemapped to SDR first; `dnr.vpy` warns but does not tonemap.
- Keep input/output matrix consistent, otherwise the whole frame shifts colour.
  BT.709 is the default; SD content often needs `DNR_MATRIX=5`.

## Performance notes

- 1080p, small model: ~39.6 fps in mpv; ~58 fps in the CLI.
- `DNR_MODEL_SCALE=0.75` → ~49.9 fps in mpv with little visual loss.
- 4K is ~8 fps; use the CLI/offline path.
- First frame pays the weight upload: ~0.6 s with a warm `pipeline.cache`, ~20 s cold.
- Keep the model root writable: the runtime writes `dlssnr-amd/pipeline.cache`.
