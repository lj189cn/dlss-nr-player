# Testing

## What was tested

All numbers in [PERFORMANCE](PERFORMANCE.md) were produced inside the reference
WolfLutris container with the scripts in `scripts/` (see [WOLFLUTRIS](WOLFLUTRIS.md)).

| Milestone | Test | Result |
|---|---|---|
| M0 | Compile DLSSNR-AMD core against Vulkan 1.4.357 headers | OK |
| M1 | Single 1080p frame (`nr_host`), 310.8.0 | 22.6 ms / 44.2 fps; PSNR 50.27 dB vs 310.8 reference |
| M2 | `dnr-video` 300 frames 1080p | 24.4 ms / 41.0 fps; output bit-exact |
| M3 | mpv + VS plugin, 300 frames | 30.5 fps (old model) |
| M3 | A/B `--vo=image` | PSNR 27.3 dB on/off, colour delta ≤1 LSB |
| M4 | Real display `gpu-next` + Vulkan | window screenshot, plugin active |
| M4 | 10 min / 18 000 frames | 0 dropped frames, VRAM constant, rc=0 |
| M4 | 4K, 120 frames | 8.16 fps in mpv, 62–67 ms network |
| M4.5 | 310.9.16 small model | 39.6 fps in mpv, 57.8 fps CLI, 50.80 dB |

## Reproduce

After the environment is built (see [BUILD](BUILD.md)):

```bash
# CLI benchmark (needs a 300-frame rawvideo; any 1080p clip works)
ffmpeg -i sample.mp4 -frames:v 300 -vf scale=1920:1080 -pix_fmt rgba -f rawvideo - \
| build/dnr-video --width 1920 --height 1080 --root "$DNR_ROOT" --bench

# mpv benchmark + screenshots
DNR_ROOT="$DNR_ROOT" DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so \
scripts/run-mpv-test.sh --sample sample.mp4 --frames 300 --out /tmp/dnr-test

# 10-minute stability (real display)
DNR_ROOT="$DNR_ROOT" DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so \
scripts/m4-stability.sh --sample sample_10min.mp4 --frames 18000

# 310.9.16 small model A/B (requires the DLL you own and DLSSNR-AMD model-tools)
CONTAINER=... DNR_ROOT=... scripts/test-model-3109.sh
```

`scripts/run-mpv-test.sh` reports what actually matters:

- plugin log lines `平均 network ...` every 100 frames (proof frames reached the kernel),
- wall-clock FPS over `--vo=null --untimed`,
- `--vo=image` screenshots for A/B.

## PSNR method

- Input: the reference captures shipped by DLSSNR-AMD
  (`docs/ngx-verification/<version>/...`).
- Output: our host/plugin at 1920×1080, RGBA8.
- Compare with FFmpeg's `psnr` filter (`ffmpeg -i a.png -i b.png -lavfi psnr -f null -`).
- 310.9.16 provides outputs only; its input is the 310.8.0 `single-frame-inputs/1920x1080.png`.
- `single-frame-images/*.png` are 2880×540 / 2160×600 three-panel montages, **not**
  1080p inputs.

## Known gaps

- Not validated on RDNA2/3 (no FP8 WMMA path in this runtime).
- Not validated on GNOME/KDE; the reference display is sway.
- `--gpu-api=opengl` aborts in mpv 0.41's X11 init (unrelated to NR); use Vulkan.
- 4K run-to-run variance is ~24% (clock/thermal); report ranges, not single runs.
- No long-duration test beyond 10 minutes; no GPU-loss/robustness test.

## Test assets

The repository intentionally does **not** ship game frames, screenshots, weights
or logs. Bring your own SDR clip and the DLSSNR-AMD verification assets.
