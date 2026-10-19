#!/usr/bin/env bash
# build-vapoursynth-mpv.sh — 从源码装 VapourSynth + 带 VS 支持的 mpv（Ubuntu 25.04）
#
# 为什么需要它：
#   · Ubuntu 25.04 / Debian 官方仓库**没有** vapoursynth / libvapoursynth-dev
#     （Launchpad 上该源码包 0 个已发布版本）；
#   · Ubuntu 的 mpv 打包时没有 vapoursynth 依赖 → 内置的 `--vf=vapoursynth` 不可用。
#
# 用法（在 WolfLutris 里，需要 sudo）：
#   scripts/build-vapoursynth-mpv.sh --prefix /usr/local
#   scripts/build-vapoursynth-mpv.sh --prefix /usr/local --skip-mpv   # 只装 VS
#
# 装完运行：
#   export VSSCRIPT_PATH="$(python3 -c 'import sysconfig;print(sysconfig.get_path("purelib"))')/vapoursynth/libvsscript.so"
#   mpv --vf=vapoursynth=./dnr.vpy movie.mkv
#
# 说明：
#   · 会 apt-get install 构建依赖；不想动系统就把第 0 节的命令抄出来手动跑。
#   · 版本：VapourSynth R81、mpv 0.41.0（都走 API 4）。
#   · mpv 的依赖清单给的是“常见集合”，若 meson setup 报缺依赖，按提示补装即可
#     （mpv 的 meson 会一次列出所有缺失项）。

set -euo pipefail

prefix="/usr/local"
jobs="$( (command -v nproc >/dev/null && nproc) || echo 4 )"
skip_mpv=0
vs_tag="${VS_TAG:-R81}"
mpv_tag="${MPV_TAG:-v0.41.0}"
src_dir="${SRC_DIR:-$HOME/dnr-src}"

usage() { sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)   prefix="$2"; shift 2 ;;
        --jobs|-j)  jobs="$2"; shift 2 ;;
        --skip-mpv) skip_mpv=1; shift ;;
        --help|-h)  usage 0 ;;
        *) echo "未知参数: $1" >&2; usage 1 ;;
    esac
done

run_sudo() { if [[ "${EUID}" -eq 0 ]]; then "$@"; else sudo "$@"; fi; }

# ------------------------------------------------------------------ apt 修复
# 坑（2026-10-19 实测）：WolfLutris 为跑 DLSSNR 用 `dlssnr-test/fix-mesa.sh` 以
# `dpkg -i --force-depends` 装了 Mesa 25.3.3（mesa-libgallium + mesa-vulkan-drivers）。
# 结果是 dpkg 状态里 libegl-mesa0/libgbm1/libglx-mesa0/mesa-va-drivers 仍钉在 25.0.7
# （`Depends: mesa-libgallium (= 25.0.7-…)`），mesa-vulkan-drivers 要求
# `libwayland-client0 >= 1.24`（容器是 1.23.1）。apt 3.0 **不允许**在这种 broken 状态下
# 安装任何东西：连 `apt install meson` 都会报 "E: Unmet dependencies"。
#
# 解法：装两个**只含元数据**的 shim 包，用版本化 `Provides` 补上这两条永远满足不了的依赖。
#   · 不替换、不动任何 Mesa 二进制 → Vulkan 扩展（coop matrix / shader_float8）不受影响
#   · i386 必须单独一个包名（同名包不能同时存在两个架构）
# 反向验证：装完 `apt-get check` 静默返回 0；`vulkaninfo | grep -c cooperative_matrix` 不变。
ensure_apt_usable() {
    if apt-get check >/dev/null 2>&1; then
        echo "   apt 依赖状态正常"
        return 0
    fi
    echo "   apt 处于 broken 状态（Mesa 25.3.3 被 --force-depends 安装）→ 装元数据 shim"
    local dir="${src_dir}/apt-shim"
    mkdir -p "${dir}"
    local name arch
    for pair in "wolfrutris-depshim-amd64 all" "wolfrutris-depshim-i386 i386"; do
        name="${pair% *}"; arch="${pair#* }"
        rm -rf "${dir}/${name}"
        mkdir -p "${dir}/${name}/DEBIAN"
        cat > "${dir}/${name}/DEBIAN/control" <<EOF
Package: ${name}
Version: 1.0
Section: misc
Priority: optional
Architecture: ${arch}
Maintainer: local <root@localhost>
Description: Metadata-only apt shim for WolfLutris (Mesa force-installed)
 Ships no files. Declares versioned Provides so apt stops treating the
 force-installed Mesa 25.3.3 as broken:
   mesa-libgallium (= 25.0.7-0ubuntu0.25.04.2)
   libwayland-client0 (= 1.24.0-1)
Provides: mesa-libgallium (= 25.0.7-0ubuntu0.25.04.2), libwayland-client0 (= 1.24.0-1)
EOF
        dpkg-deb -b "${dir}/${name}" "${dir}/${name}.deb" >/dev/null
        run_sudo dpkg -i "${dir}/${name}.deb"
    done
    if ! apt-get check >/dev/null 2>&1; then
        echo "错误: apt 仍然 broken，请人工检查（dpkg -l | grep mesa；apt-get check）" >&2
        exit 1
    fi
    echo "   apt 已恢复；Mesa 二进制未改动"
}

echo "== 0. 构建依赖 =="
run_sudo apt-get update
ensure_apt_usable
run_sudo apt-get install -y --no-install-recommends \
    build-essential ninja-build meson pkg-config git ca-certificates curl \
    python3-dev cython3 \
    libzimg-dev libvulkan-dev vulkan-tools

# 只装 VS 时到此为止；装 mpv 的依赖（失败不致命，meson 会指出缺什么）
if [[ "${skip_mpv}" -eq 0 ]]; then
    run_sudo apt-get install -y --no-install-recommends \
        libplacebo-dev libass-dev libavcodec-dev libavformat-dev libavutil-dev \
        libswscale-dev libswresample-dev libavfilter-dev libavdevice-dev \
        libluajit-5.1-dev libuchardet-dev libjpeg-dev libarchive-dev \
        libdrm-dev libgbm-dev libegl-dev libgl-dev libpulse-dev \
        libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxpresent-dev \
        libwayland-dev wayland-protocols libmujs-dev libcaca-dev \
        libdvdnav-dev libbluray-dev libvdpau-dev libva-dev libsdl2-dev
fi

command -v meson >/dev/null || { echo "meson 未安装（需要 >= 1.3）" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja 未安装" >&2; exit 1; }

mkdir -p "${src_dir}"

# ------------------------------------------------------------------ Cython
# 坑 2（2026-10-19 实测）：plucky 的 cython3 是 3.0.11，不认 VS R81 生成的
# `-X freethreading_compatible=True`（Cython 3.1 才有的 directive），
# 报 `ValueError: Unknown option: "freethreading_compatible"`。用 pip 装 >= 3.1
# （落到 /usr/local/bin，先于 /usr/bin 生效；Ubuntu 25.04 是 PEP 668 externally-managed，
#  所以要 --break-system-packages）。
cython_ok() { cython --version 2>&1 | sed -n 's/^Cython version //p' | awk -F. '{ exit !(($1 > 3) || ($1 == 3 && $2 >= 1)) }'; }
if ! cython_ok; then
    echo "== 0.1 Cython >= 3.1（系统自带 $(cython --version 2>&1 | tail -1) 太旧） =="
    run_sudo python3 -m pip install --break-system-packages --quiet "cython>=3.1"
    hash -r
    cython_ok || { echo "错误: Cython 仍 < 3.1，VS 无法生成 cython 绑定" >&2; exit 1; }
fi
echo "   Cython: $(cython --version 2>&1 | tail -1)"

# ------------------------------------------------------------------ VS
vs_src="${src_dir}/vapoursynth-${vs_tag}"
echo "== 1. VapourSynth ${vs_tag} =="
if [[ ! -d "${vs_src}" ]]; then
    curl -fL -o "${src_dir}/vs.tar.gz" \
        "https://codeload.github.com/vapoursynth/vapoursynth/tar.gz/refs/tags/${vs_tag}"
    mkdir -p "${vs_src}"
    tar xzf "${src_dir}/vs.tar.gz" --strip-components=1 -C "${vs_src}"
fi

# 坑 3（2026-10-19 实测）：GCC 14 + Vulkan-Headers 1.4.304 下 R81 编译失败：
#   src/core/gpufilter.h:1124  error: invalid conversion from 'const void*' to 'void*'
#   （VkPipelineShaderStageRequiredSubgroupSizeCreateInfo::pNext 在 vulkan_core.h 里
#     是 `void*` 而不是 `const void*`，属于 Vulkan-Headers 的历史遗留签名）
# 只加一个 const_cast；pNext 链对驱动只读，语义不变。幂等（已改过就跳过）。
patch_vs() {
    local f="${vs_src}/src/core/gpufilter.h"
    [[ -f "${f}" ]] || return 0
    if grep -q 'reqSg.pNext = pipeInfo.stage.pNext;' "${f}"; then
        sed -i 's|reqSg\.pNext = pipeInfo\.stage\.pNext;|/* GCC14+Vulkan-Headers 1.4.304: 该结构体的 pNext 是 void*，加 const_cast */\n            reqSg.pNext = const_cast<void *>(pipeInfo.stage.pNext);|' "${f}"
        echo "   补丁: gpufilter.h 的 reqSg.pNext 加 const_cast（GCC 14 严格转换）"
    fi
}
patch_vs

(
    cd "${vs_src}"
    # VS 把库/头文件/python 模块/pkgconfig 全部装进 <site-packages>/vapoursynth
    if [[ -f build/meson-private/coredata.dat ]]; then
        meson setup --reconfigure build --prefix="${prefix}" --buildtype=release
    else
        meson setup build --prefix="${prefix}" --buildtype=release
    fi
    ninja -C build -j "${jobs}"
    run_sudo meson install -C build
)

purelib="$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')"
vs_dir="${purelib}/vapoursynth"

# 坑 4（2026-10-19 实测）：Debian/Ubuntu 打过补丁的 Python 里有 `deb_system` scheme，
# meson 的 python 模块在 `--prefix /usr/local` 下会选它，把封装到
#   /usr/local/lib/python3/dist-packages/vapoursynth      ← version-agnostic
# 而不是 sys.path 里的
#   /usr/local/lib/python3.13/dist-packages/vapoursynth
# 结果 `import vapoursynth` 直接 ModuleNotFoundError。这里不改 meson，只补一行 .pth
# 把这个目录加进 sys.path（幂等），然后把 vs_dir 指向真实安装位置。
vs_alt="${prefix}/lib/python3/dist-packages/vapoursynth"
if [[ ! -f "${vs_dir}/libvsscript.so" && -f "${vs_alt}/libvsscript.so" ]]; then
    echo "${prefix}/lib/python3/dist-packages" \
        | run_sudo tee "${purelib}/vapoursynth-local.pth" >/dev/null
    echo "   安装位置修正: ${vs_alt}（已加 ${purelib}/vapoursynth-local.pth）"
    vs_dir="${vs_alt}"
fi
run_sudo install -d "${vs_dir}/plugins"

echo "   VS 安装目录: ${vs_dir}"
[[ -f "${vs_dir}/libvsscript.so" ]] || {
    echo "警告: 没看到 ${vs_dir}/libvsscript.so（检查 --prefix 与 python3 是否匹配）" >&2; }
python3 -c 'import vapoursynth as vs; print("   vapoursynth:", vs.core.version())' || {
    echo "错误: Python 导入 vapoursynth 失败（检查 PYTHONPATH）" >&2; exit 1; }
export PKG_CONFIG_PATH="${vs_dir}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
pkg-config --modversion vapoursynth >/dev/null 2>&1 || {
    echo "错误: pkg-config 找不到 vapoursynth（找 ${vs_dir}/pkgconfig/vapoursynth.pc）" >&2; exit 1; }
vs_ver="$(pkg-config --modversion vapoursynth)"
echo "   pkg-config vapoursynth: ${vs_ver}"

# 坑 5（2026-10-19 实测）：VS R81 的 meson 只装 `vapoursynth.pc`，**不装**
# `vapoursynth-script.pc`；而 mpv 0.41 的 meson.build 要
#   dependency('vapoursynth-script', version: '>= 56')
# 于是 configure 直接 `ERROR: Dependency "vapoursynth-script" not found`。
#
# 另一个实测修正：**mpv 0.41 不是 dlopen libvsscript**。`video/filter/vf_vapoursynth.c:807`
# 直接调用 `getVSScriptAPI(VSSCRIPT_API_VERSION)`（整个源码里没有 `VSSCRIPT_PATH`），
# 所以必须真链接 `-lvsscript`；`04_VapourSynth与mpv集成.md` §1 里"dlopen + VSSCRIPT_PATH"
# 那条只适用于旧版 mpv（0.3x），对 0.41 不成立。
# → .pc 里必须给 `Libs: -L${libdir} -lvsscript`，同时把 libvsscript.so 放进 ld 搜索路径，
#   否则 mpv 起不来（`error while loading shared libraries: libvsscript.so`）。
if [[ ! -f "${vs_dir}/pkgconfig/vapoursynth-script.pc" ]]; then
    pc_existed=0; else pc_existed=1; fi
cat <<EOF | run_sudo tee "${vs_dir}/pkgconfig/vapoursynth-script.pc" >/dev/null
prefix=\${pcfiledir}/..
libdir=\${prefix}
includedir=\${prefix}/include

Name: vapoursynth-script
Description: VapourSynth script library (mpv 0.41 links getVSScriptAPI directly)
Version: ${vs_ver}
Libs: -L\${libdir} -lvsscript
Cflags: -I\${includedir}
EOF
[[ "${pc_existed}" -eq 1 ]] || echo "   补 vapoursynth-script.pc（mpv meson 需要；含 -lvsscript）"
pkg-config --modversion vapoursynth-script >/dev/null 2>&1 \
    && echo "   pkg-config vapoursynth-script: $(pkg-config --modversion vapoursynth-script)"

# 运行期：mpv 链接了 libvsscript.so（NEEDED），必须能在默认 ld 路径里找到。
#
# 坑 6（2026-10-19 实测，最隐蔽）：**不要用软链**。VapourSynth R81 的 vsscript 靠
# `dladdr()` 拿到自己的真实路径，再去 `~/.config/vapoursynth/vapoursynth.toml` 里查
# "这个 libvsscript.so 对应哪个 python 解释器 + libpython"（见 src/vsscript/vsscript.cpp
# 的 getLibraryPath()/readEnvConfig()）。若 /usr/local/lib/libvsscript.so 是指向真实文件的
# 软链，dladdr 返回的是软链路径，与 config 里的键（真实路径）对不上，于是：
#   getVSScriptAPI() → NULL，lastError = "Python executable and library path couldn't be
#   determined despite automatic configuration. Run `vapoursynth config` to set it..."
# 而 mpv 只会打一行 "Could not initialize VapourSynth scripting."，极难定位。
# → 正确做法：把**真实目录**写进 /etc/ld.so.conf.d，让 ld.so 直接用真实路径加载。
printf '%s\n' "${vs_dir}" | run_sudo tee /etc/ld.so.conf.d/vapoursynth.conf >/dev/null
run_sudo ldconfig
# 注意：脚本开了 pipefail，`ldconfig -p | grep -q` 会因 grep 提前退出判成失败，先落变量。
ld_out="$(ldconfig -p 2>/dev/null || true)"
if grep -q 'libvsscript\.so' <<<"${ld_out}"; then
    echo "   ldconfig: $(grep -m1 'libvsscript\.so' <<<"${ld_out}")"
else
    echo "警告: ldconfig 看不到 libvsscript.so，mpv 运行时需要 LD_LIBRARY_PATH=${vs_dir}" >&2
fi

# 坑 6 的另一半：vsscript 需要一份运行时配置（python 解释器 + libpython 路径）。
# `python3 -m vapoursynth config` 会写到 ${HOME}/.config/vapoursynth/vapoursynth.toml，
# **是按用户/按 HOME 的**：用另一个用户（或不同 HOME）跑 mpv 时要再跑一次。
python3 -m vapoursynth config 2>&1 | sed 's/^/   /' || true
vs_check="$(python3 -m vapoursynth check-env 2>&1 || true)"
printf '%s\n' "${vs_check}" | sed 's/^/   /'
if grep -qi "NOT CONFIGURED" <<<"${vs_check}"; then
    echo "警告: vapoursynth 运行时配置没写好，mpv 会报 'Could not initialize VapourSynth scripting.'" >&2
fi

if [[ "${skip_mpv}" -eq 1 ]]; then
    echo "== 2. 跳过 mpv（--skip-mpv） =="
    echo "   手动构建 mpv："
    echo "   PKG_CONFIG_PATH=${vs_dir}/pkgconfig meson setup build -Dvapoursynth=enabled"
    exit 0
fi

# ------------------------------------------------------------------ mpv
mpv_src="${src_dir}/mpv-${mpv_tag#v}"
echo "== 2. mpv ${mpv_tag}（-Dvapoursynth=enabled） =="
if [[ ! -d "${mpv_src}" ]]; then
    curl -fL -o "${src_dir}/mpv.tar.gz" \
        "https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/${mpv_tag}"
    mkdir -p "${mpv_src}"
    tar xzf "${src_dir}/mpv.tar.gz" --strip-components=1 -C "${mpv_src}"
fi
(
    cd "${mpv_src}"
    export PKG_CONFIG_PATH="${vs_dir}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    pkg-config --exists vapoursynth || {
        echo "错误: pkg-config 找不到 vapoursynth（找 ${vs_dir}/pkgconfig/vapoursynth.pc）" >&2
        exit 1
    }
    echo "   pkg-config vapoursynth: $(pkg-config --modversion vapoursynth)"
    if [[ -f build/meson-private/coredata.dat ]]; then
        meson setup --reconfigure build --prefix="${prefix}" --buildtype=release \
            -Dvapoursynth=enabled -Dlibmpv=false
    else
        meson setup build --prefix="${prefix}" --buildtype=release \
            -Dvapoursynth=enabled -Dlibmpv=false
    fi
    ninja -C build -j "${jobs}"
    run_sudo meson install -C build
)

echo "== 3. 完成 =="
cat <<EOF

mpv 0.41 直接链接 libvsscript（不是 dlopen），脚本已写入：
  /etc/ld.so.conf.d/vapoursynth.conf  ($vs_dir)
并运行 ldconfig；vsscript 的运行时配置写在 ~/.config/vapoursynth/vapoursynth.toml。

自检：
  mpv --version | head -3
  mpv --vf=help 2>&1 | grep -i vapoursynth || true

播放：
  export DNR_ROOT=/path/to/dlssnr-root
  export DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so
  mpv --vo=gpu-next --gpu-api=vulkan --vf=vapoursynth="$PWD/dnr.vpy" movie.mkv
EOF
