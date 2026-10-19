# third_party/vapoursynth — 随仓库附带的 VapourSynth API 4 头文件

来源：<https://github.com/vapoursynth/vapoursynth>，tag **R81**（2026-10-04），`include/` 目录。

| 文件 | 用途 |
|---|---|
| `VapourSynth4.h` | 插件/滤镜 API（`VSAPI`、`VSPLUGINAPI`、`VSMap`、`createVideoFilter`、`getFrame`…） |
| `VSHelper4.h` | `vsh::` 小工具（`isConstantVideoFormat` 等） |
| `VSScript4.h` | VSScript API（mpv 的 `--vf=vapoursynth` 通过它加载脚本；插件本身不直接用） |
| `VSConstants4.h` | 常量 |
| `COPYING.LESSER` | LGPL-2.1（VapourSynth 许可证） |

## 为什么要自带

Ubuntu 25.04 / Debian 13 **都没有** `vapoursynth` / `libvapoursynth-dev` 包
（见 <https://launchpad.net/ubuntu/+source/vapoursynth>，官方存档中 0 个已发布版本；
`packages.ubuntu.com` 与 `packages.debian.org` 均查无此包）。

- VapourSynth 4 插件**不需要链接** `libvapoursynth`：宿主通过
  `VapourSynthPluginInit2(VSPlugin*, const VSPLUGINAPI*)` 把函数表传进来，
  所以**只有头文件就能编译插件**，不需要在本机安装 VS。
- 运行期才需要 VS（`vapoursynth` Python 模块 + `libvsscript.so`）和带 VS 支持的 mpv。

## 版本兼容

插件只用到 API 4.0 的函数（`vsapi->` 里 R4 就有的一批），因此：

- 用任意 API 4.x 版本的 VS（R4x–R8x）都能加载；
- 若在 WolfLutris 上装了系统头文件（`/usr/include/vapoursynth/VapourSynth4.h`），
  CMake 可用 `-DVAPOURSYNTH_INCLUDE_DIR=...` 指向它；
  默认优先用本目录的头文件，保证与 R81 行为一致、构建可复现。

## 升级方法

```bash
# 例：升到 R82
curl -fL -o /tmp/vs.tar.gz https://codeload.github.com/vapoursynth/vapoursynth/tar.gz/refs/tags/R82
tar xzf /tmp/vs.tar.gz --strip-components=1 -C /tmp/vs82
cp /tmp/vs82/include/{VapourSynth4.h,VSHelper4.h,VSScript4.h,VSConstants4.h} third_party/vapoursynth/include/
cp /tmp/vs82/COPYING.LESSER third_party/vapoursynth/
```
