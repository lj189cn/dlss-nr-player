#!/usr/bin/env bash
# build-core.sh — 构建 libdnr（C ABI）与 dnr-video CLI
#
# 前置：
#   1) 一份 DLSSNR-AMD 源码（MIT）：默认 third_party/DLSSNR-AMD，或 DLSSNR_AMD_SRC=...
#   2) Vulkan 头（>= 1.4.304，需 VK_EXT_shader_float8 / VK_KHR_cooperative_matrix）：
#      默认用系统 /usr/include，或 VULKAN_HEADERS=/path/to/Vulkan-Headers/include
#   3) g++ / libvulkan（运行期）
#
# 用法：
#   DLSSNR_AMD_SRC=/path/to/DLSSNR-AMD-main scripts/build-core.sh
#   VULKAN_HEADERS=/opt/Vulkan-Headers/include scripts/build-core.sh
#
# 产物：
#   build/dnr-video   裸 RGBA8 管道 CLI
#   build/libdnr.so   C ABI，给 VapourSynth 插件链接
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
DLSSNR_AMD_SRC=${DLSSNR_AMD_SRC:-$ROOT/third_party/DLSSNR-AMD}
BUILD=${BUILD:-$ROOT/build}
CXX=${CXX:-g++}
JOBS=${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}

die() { echo "error: $*" >&2; exit 1; }

[[ -f "$DLSSNR_AMD_SRC/linux/src/core/nr_runtime.cpp" ]] || die \
    "找不到 DLSSNR-AMD 源码：$DLSSNR_AMD_SRC（用 DLSSNR_AMD_SRC=... 指定，或放到 third_party/DLSSNR-AMD）"

# Vulkan 头路径：优先显式 VULKAN_HEADERS，其次 pkg-config，最后系统路径
if [[ -n "${VULKAN_HEADERS:-}" ]]; then
    VKINC=$VULKAN_HEADERS
elif pkg-config --exists vulkan 2>/dev/null; then
    VKINC=$(pkg-config --cflags-only-I vulkan | sed 's/^-I//; s/ .*//')
elif [[ -f /usr/include/vulkan/vulkan.h ]]; then
    VKINC=/usr/include
else
    die "找不到 vulkan/vulkan.h：装 libvulkan-dev，或 VULKAN_HEADERS=/path/to/include"
fi
[[ -f "$VKINC/vulkan/vulkan.h" ]] || die "VULKAN_HEADERS 下没有 vulkan/vulkan.h：$VKINC"

# 上游的 RDNA4 编译常量（必须与 shader 一致；nr_graph 会校验 shader-constants.txt）
# shellcheck disable=SC1091
source "$DLSSNR_AMD_SRC/linux/build/arch/rdna4.sh"
: "${NR_PRODUCT_DEFINES:?rdna4.sh 未提供 NR_PRODUCT_DEFINES}"

CORE=$DLSSNR_AMD_SRC/linux/src/core
INC=(-I"$VKINC" -I"$DLSSNR_AMD_SRC/linux/src" -I"$CORE" -I"$ROOT/src")
CXXFLAGS=(-std=c++17 -O2 -DNDEBUG -Wall -Wextra)
mkdir -p "$BUILD/obj" "$BUILD/shared"

echo "== 源: $DLSSNR_AMD_SRC"
echo "== Vulkan 头: $VKINC"
echo "== 构建目录: $BUILD"

echo "== 编译 core（可执行文件，非 PIC）"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" "${NR_PRODUCT_DEFINES[@]}" \
    -c "$CORE/nr_runtime.cpp" -o "$BUILD/obj/nr_runtime.o"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" \
    -c "$CORE/nr_native_plan.cpp" -o "$BUILD/obj/nr_native_plan.o"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" \
    -c "$CORE/nr_renodx.cpp" -o "$BUILD/obj/nr_renodx.o"

echo "== 编译 CLI"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" -c "$ROOT/src/dnr_core.cpp" -o "$BUILD/obj/dnr_core.o"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" -c "$ROOT/src/dnr_video.cpp" -o "$BUILD/obj/dnr_video.o"
"$CXX" -o "$BUILD/dnr-video" \
    "$BUILD/obj/dnr_core.o" "$BUILD/obj/dnr_video.o" \
    "$BUILD/obj/nr_runtime.o" "$BUILD/obj/nr_native_plan.o" "$BUILD/obj/nr_renodx.o" \
    -lvulkan

echo "== 编译 PIC core + libdnr.so"
"$CXX" "${CXXFLAGS[@]}" -fPIC "${INC[@]}" "${NR_PRODUCT_DEFINES[@]}" \
    -c "$CORE/nr_runtime.cpp" -o "$BUILD/shared/nr_runtime.o"
"$CXX" "${CXXFLAGS[@]}" -fPIC "${INC[@]}" \
    -c "$CORE/nr_native_plan.cpp" -o "$BUILD/shared/nr_native_plan.o"
"$CXX" "${CXXFLAGS[@]}" -fPIC "${INC[@]}" \
    -c "$CORE/nr_renodx.cpp" -o "$BUILD/shared/nr_renodx.o"
"$CXX" "${CXXFLAGS[@]}" -fPIC "${INC[@]}" -c "$ROOT/src/dnr_core.cpp" -o "$BUILD/shared/dnr_core.o"
"$CXX" -shared -o "$BUILD/libdnr.so" \
    "$BUILD/shared/dnr_core.o" \
    "$BUILD/shared/nr_runtime.o" "$BUILD/shared/nr_native_plan.o" "$BUILD/shared/nr_renodx.o" \
    -lvulkan

echo "== 完成"
ls -lh "$BUILD/dnr-video" "$BUILD/libdnr.so"
echo
echo "插件构建：DNR_LIBRARY=$BUILD/libdnr.so scripts/build-vs-plugin.sh --prefix /usr/local"
echo "运行 CLI：$BUILD/dnr-video --width W --height H --root <dlssnr root> --in in.rgba --out out.rgba"
