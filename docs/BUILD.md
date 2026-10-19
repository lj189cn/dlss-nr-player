# Build

Two independent pieces:

1. **core + plugin** (this repo) — needs DLSSNR-AMD source and Vulkan headers;
2. **player stack** — VapourSynth R81 + mpv 0.41 with `-Dvapoursynth=enabled`
   (Ubuntu/Debian do not package either; the script builds both).

## Dependencies

```bash
# Debian/Ubuntu
sudo apt install -y build-essential cmake ninja-build pkg-config git curl \
    libvulkan-dev vulkan-tools python3

# mpv/VapourSynth build (script installs these too)
sudo apt install -y meson python3-dev cython3 libzimg-dev \
    libplacebo-dev libass-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libavfilter-dev libavdevice-dev \
    libluajit-5.1-dev libuchardet-dev libjpeg-dev libarchive-dev libdrm-dev \
    libgbm-dev libegl-dev libgl-dev libpulse-dev libx11-dev libxext-dev \
    libxfixes-dev libxrandr-dev libxpresent-dev libwayland-dev \
    wayland-protocols libmujs-dev libcaca-dev libdvdnav-dev libbluray-dev \
    libvdpau-dev libva-dev libsdl2-dev
```

Vulkan headers must be **1.4.304+** (they need `VkPhysicalDeviceShaderFloat8FeaturesEXT`).
Ubuntu 25.04's `libvulkan-dev` 1.4.304 works; older distros should use
[Vulkan-Headers v1.4.357](https://github.com/KhronosGroup/Vulkan-Headers):

```bash
git clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers.git /opt/Vulkan-Headers
```

## 1. DLSSNR-AMD source

```bash
git clone https://github.com/mochizuki0323/DLSSNR-AMD.git /opt/DLSSNR-AMD
```

`scripts/build-core.sh` compiles three files from it:

- `linux/src/core/nr_runtime.cpp` (includes `nr_graph.cpp`; do not compile separately)
- `linux/src/core/nr_native_plan.cpp`
- `linux/src/core/nr_renodx.cpp`

The RDNA4 constants come from `linux/build/arch/rdna4.sh`; they must match the
SPIR-V (`shader-constants.txt`), otherwise the runtime refuses the shader directory.

## 2. Model root

You need the shader tree and weights from the DLSSNR-AMD **Linux package**
(`DLSSNR-AMD-Vulkan-Linux-*.tar.gz`), installed for your own DLL:

```
/opt/dlssnr-root/
└── dlssnr-amd/
    ├── dlssnr.bin          # 310.8.0 / 310.9.16 original preset
    ├── dlssnr-small.bin    # optional, 310.9.16 small/new model (recommended)
    └── shaders/            # g_*.spv, runtime/, temporal/, shader-constants.txt
```

The package's `model-tools/extract_model.sh` takes your `nvngx_dlssnr.dll`
(310.8.0 or 310.9.16) and verifies every weight entry. The repository does not
include NVIDIA binaries or weights.

## 3. Build core + CLI + libdnr

```bash
DLSSNR_AMD_SRC=/opt/DLSSNR-AMD \
VULKAN_HEADERS=/usr/include \
scripts/build-core.sh
```

Outputs:

```
build/dnr-video     # rawvideo CLI
build/libdnr.so     # C ABI, link target for the plugin
```

## 4. Build the VapourSynth plugin

```bash
DNR_LIBRARY=$PWD/build/libdnr.so \
scripts/build-vs-plugin.sh --prefix /usr/local
```

This produces `/usr/local/lib/vapoursynth/dnr.so`. The plugin has
`RUNPATH=$ORIGIN`; if `libdnr.so` is not installed next to it:

```bash
sudo cp build/libdnr.so /usr/local/lib/vapoursynth/
# or: export LD_LIBRARY_PATH=$PWD/build:$LD_LIBRARY_PATH
```

`scripts/build-vs-plugin.sh --stub` builds a pass-through plugin (no GPU) useful for
testing the VS/mpv plumbing.

## 5. Build VapourSynth R81 + mpv 0.41

```bash
sudo scripts/build-vapoursynth-mpv.sh --prefix /usr/local
```

What it does:

- installs build deps;
- installs Cython ≥3.1 if the distro's is older (VS R81 needs it);
- builds VapourSynth R81, installs to `<site-packages>/vapoursynth`;
- adds the real path to `/etc/ld.so.conf.d/vapoursynth.conf` + `ldconfig`
  (mpv 0.41 links `libvsscript`, it does not dlopen it);
- generates `~/.config/vapoursynth/vapoursynth.toml` via `python3 -m vapoursynth config`;
- builds mpv 0.41 with `-Dvapoursynth=enabled`.

Verify:

```bash
mpv --vf=help | grep vapoursynth
python3 -c 'import vapoursynth as vs; print(vs.core.version())'
ldd /usr/local/bin/mpv | grep vsscript
```

## 6. Verify the plugin without a player

```bash
python3 - <<'PY'
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin(path="/usr/local/lib/vapoursynth/dnr.so")
print([p for p in core.plugins() if p.namespace == "dnr"])
PY
```

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `VkPhysicalDeviceShaderFloat8FeaturesEXT` missing at compile time | Vulkan headers too old; use 1.4.304+ or `/opt/Vulkan-Headers`. |
| Runtime dies with `e4m3 x e4m3 -> fp32 ... subgroup config not reported` | It picked the iGPU. Set `NR_GPU=<index of the discrete AMD device>`; enumerate with `vulkaninfo`. |
| `adapter shaders must be inside NR root` | `DNR_ROOT` / `root` is wrong; point it at the directory containing `dlssnr-amd/`. |
| Plugin loads but no effect | Check `mpv --vf=help`, plugin `stderr` (`[dnr] 内核就绪`), and that `dnr.vpy` ends with a YUV output. |
| `Could not initialize VapourSynth scripting` | `libvsscript.so` not in ld.so path, or `vapoursynth.toml` missing; see step 5. |
| `Resize error 3074: no path between colorspaces` | The script must set `matrix_in` explicitly (VS R81 no longer infers it). |
| `Unsupported output format` / filter disabled | mpv's VS bridge only accepts YUV; the script converts RGB24→YUV before `set_output()`. |
| `wl_display_dispatch_queue_timeout` undefined | Mesa built for libwayland ≥1.24; upgrade libwayland (see [WOLFLUTRIS](WOLFLUTRIS.md)). |

## WolfLutris

The reference test environment (custom Mesa 25.3.3, libwayland 1.24, apt shims)
is documented in [WOLFLUTRIS.md](WOLFLUTRIS.md), including the exact order to
restore the container after a rebuild.
