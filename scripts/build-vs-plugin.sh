#!/usr/bin/env bash
# build-vs-plugin.sh — 构建并（默认）安装 VapourSynth 插件 dnr.so
#
# 用法：
#   scripts/build-vs-plugin.sh                  # 自动找 libdnr，找不到则直通占位
#   scripts/build-vs-plugin.sh --stub           # 强制直通占位（只验证集成）
#   scripts/build-vs-plugin.sh --library /home/retro/dnr/libdnr.so --prefix "$HOME/.local"
#   DNR_LIBRARY=/path/libdnr.so scripts/build-vs-plugin.sh
#
# 环境：
#   VAPOURSYNTH_INCLUDE_DIR  覆盖 VapourSynth 头文件目录
#   BUILD_DIR                构建目录（默认 <repo>/build）
#   JOBS                     并行度（默认 nproc）
#
# 退出码：0 成功；非 0 表示配置/编译失败（脚本会打印 cmake 原始错误）。

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

prefix="${PREFIX:-$HOME/.local}"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
jobs="${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}"
use_stub=0
library="${DNR_LIBRARY:-}"
extra_cmake=()

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stub)      use_stub=1; shift ;;
        --library)   library="${2:?--library 需要路径}"; shift 2 ;;
        --prefix)    prefix="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        -j|--jobs)   jobs="$2"; shift 2 ;;
        -h|--help)   usage 0 ;;
        --)          shift; extra_cmake+=("$@"); break ;;
        *)           echo "未知参数: $1" >&2; usage 1 ;;
    esac
done

die() { echo "错误: $*" >&2; exit 1; }

command -v cmake >/dev/null || die "缺少 cmake（apt install cmake）"
command -v c++ >/dev/null || command -v g++ >/dev/null || die "缺少 C++ 编译器（apt install build-essential）"

[[ -f "${repo_dir}/CMakeLists.txt" ]] || die "在 ${repo_dir} 找不到 CMakeLists.txt"
[[ -f "${repo_dir}/third_party/vapoursynth/include/VapourSynth4.h" ]] \
    || echo "警告: 仓库内没有 third_party/vapoursynth/include/VapourSynth4.h，将依赖系统头文件" >&2

cmake_args=(
    -S "${repo_dir}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="${prefix}"
)

if [[ -n "${VAPOURSYNTH_INCLUDE_DIR:-}" ]]; then
    cmake_args+=("-DVAPOURSYNTH_INCLUDE_DIR=${VAPOURSYNTH_INCLUDE_DIR}")
fi

if [[ "${use_stub}" -eq 1 ]]; then
    cmake_args+=("-DDNR_USE_STUB=ON")
elif [[ -n "${library}" ]]; then
    [[ -f "${library}" ]] || die "libdnr 不存在: ${library}"
    cmake_args+=("-DDNR_LIBRARY=${library}")
fi

if [[ ${#extra_cmake[@]} -gt 0 ]]; then
    cmake_args+=("${extra_cmake[@]}")
fi

echo "== 配置 =="
echo "   repo        : ${repo_dir}"
echo "   build       : ${build_dir}"
echo "   prefix      : ${prefix}"
echo "   libdnr      : ${library:-（自动查找 / 直通占位）}"
echo "   stub        : ${use_stub}"
cmake "${cmake_args[@]}"

echo "== 编译 (-j${jobs}) =="
cmake --build "${build_dir}" --parallel "${jobs}"

echo "== 安装 =="
cmake --install "${build_dir}"

plugin="${prefix}/lib/vapoursynth/dnr.so"
echo
echo "== 完成 =="
if [[ -f "${plugin}" ]]; then
    echo "   插件: ${plugin}"
else
    echo "   插件产物: ${build_dir}/dnr.so（安装路径可能被 DNR_VS_PLUGIN_INSTALL_DIR 改写）"
fi
cat <<'EOF'

自检（不需要 GPU，stub 也能过）：
  python3 - <<'PY'
  import vapoursynth as vs
  print(vs.core.version())
  print([p for p in vs.core.plugins() if p.namespace == "dnr"])
  PY

在脚本里手动加载（autoload 目录不是默认路径时）：
  core.std.LoadPlugin(path="/path/to/dnr.so")

mpv 播放：
  DNR_ROOT=/home/retro/dlssnr-test mpv --vf=vapoursynth=./dnr.vpy movie.mkv
EOF
