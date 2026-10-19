# Performance

Test platform (reference):

| Item | Value |
|---|---|
| GPU | AMD Radeon RX 9060 XT, RADV GFX1200 (gfx1200) |
| Driver | Mesa 25.3.3+git (`VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`) |
| CPU / RAM | 16 vCPU, container |
| Weights | 310.8.0 `dlssnr.bin`; 310.9.16 `dlssnr-small.bin` (default) |
| Player | mpv 0.41.0 (source, `-Dvapoursynth=enabled`), VapourSynth R81 |
| Plugin | `dnr.so` + `libdnr.so` (this repo) |

## 1080p (1920×1080, 300 frames)

| Path | Model | network | total / frame | FPS |
|---|---|---|---|---|
| `dnr-video` CLI | 310.9.16 small | **10.19 ms** | **17.29 ms** | **57.8** |
| `dnr-video` CLI | 310.8.0 original | 16.93 ms | 24.34 ms | 41.1 |
| mpv + plugin | 310.9.16 small | 10.32 ms | — | **39.6** |
| mpv + plugin | 310.8.0 original | 17.18 ms | — | 30.6 |
| mpv + plugin, `DNR_TEMPORAL=0` | small | 10.17 ms | — | 41.5 |
| mpv + plugin, `DNR_MODEL_SCALE=0.75` | small | 6.37 ms | — | **49.9** |

`total` for the CLI includes upload/readback/fence; mpv's wall-clock overhead
(≈8 ms/frame) is VS frame copies and mpv scheduling, not the network.

### Where the 1080p time goes (mpv, small)

| Stage | ms |
|---|---|
| Network (runtime-reported) | 10.3 |
| Upload + readback (8-bit, cached staging) | ~4.7 |
| Submit + fence | ~2.7 |
| VSFrame ↔ RGBA pack/unpack | ~3 |
| YUV↔RGB (zimg, both directions) | ~3.4 |
| mpv / VS scheduling | ~1–2 |

The network is now **not** the bottleneck; CPU↔GPU copies and colour conversions
are ~1/3 of the frame budget. Pipelining 2–3 frames is the next step.

## 4K (3840×2160)

| Path | network | total / frame | FPS |
|---|---|---|---|
| `dnr-video --bench`, 120 frames | 62–65 ms (52–65 run-to-run) | 86–91 ms | 11–14 |
| mpv + plugin, 120 frames | 67.07 ms | 122.5 ms | **8.16** |

VRAM: +2354 MiB at 4K (activation arena 1546 MB + weights 224 MB + images);
1080p is +700–1001 MiB. 4K is not real-time on this GPU; it is supported for
offline conversion and as a data point.

## Stability

1080p real-time mpv, 600 s / 18 000 frames:

- 17 900 frames entered the network (boundary off-by-100),
- `vf_fps` constant 30.0, `frame-drop-count = 0`, `decoder-frame-drop-count = 0`,
- VRAM constant at +1001 MiB, RSS 845–967 MiB (no growth),
- runtime rc=0, no crash or corruption.

## Build / cold-start

| Item | Time |
|---|---|
| First runtime build (24 pipelines, no cache) | ~20–23 s |
| Warm `pipeline.cache` | 0.61–0.85 s |
| VapourSynth R81 build (16 cores) | 66 s |
| mpv 0.41 build (16 cores) | 14 s |

Keep the runtime alive for the whole player session; do not rebuild per file.

## Quality

| Comparison | PSNR |
|---|---|
| small output vs 310.9.16 `dlssnr-amd` reference | **50.80 dB** |
| small vs NVIDIA 310.9.16 reference | 50.57 dB |
| original (310.8.0) vs 310.8.0 reference | 50.27 dB |
| input vs 310.9.16 reference | 28.71 dB |

Measured on `docs/ngx-verification` material from DLSSNR-AMD at 1920×1080.
The implementation is not bit-exact with NVIDIA's DLL (different SPIR-V layout,
FP8 scheduling), but the residual is at the ±1 LSB level.
