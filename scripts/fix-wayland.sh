#!/usr/bin/env bash
# fix-wayland.sh — 修 WolfLutris 的 Wayland 显示路径（libwayland 1.23.1 → 1.24.0）
#
# 为什么需要：
#   Mesa 25.3.3 的 `libvulkan_radeon.so` 需要 `wl_display_dispatch_queue_timeout`，
#   这个符号是 **libwayland 1.24** 才有的（1.23.1 只有 `..._pending`）。
#   容器是用 `dlssnr-test/fix-mesa.sh`（`--force-depends`）装的 Mesa，libwayland 仍停在 plucky
#   的 1.23.1，于是：
#     mpv --vo=gpu-next --gpu-api=vulkan
#       → mpv: symbol lookup error: libvulkan_radeon.so: undefined symbol:
#               wl_display_dispatch_queue_timeout
#   headless compute（dnr-video / --vo=null）不需要 Wayland surface，所以之前一直是好的。
#
# 来源：Ubuntu **questing (25.10)** 的 `wayland 1.24.0-1build1`（plucky 没有更新的）。
#   只要 client/server/egl/cursor 四个运行时库 + bin/dev（dev 是精确版本依赖）；i386 一起升，
#   免得 32 位 Vulkan 客户端（Steam 那一侧）踩同样的坑。
#   `libwayland-bin`(wayland-scanner) 1.24 依赖 questing 的 `libxml2-16 (>= 2.14.1)`（新 soname，
#   与 plucky 的 libxml2 2.12 的 `libxml2.so.2` 并存，不冲突）。
#
# 回退（脚本会先下载好旧包）：
#   cd /home/retro/wayland-rollback/<旧版本> && dpkg -i *.deb && ldconfig
#
# 用法（容器内 root）：
#   bash repo/scripts/fix-wayland.sh              # 升级 + 验证
#   bash repo/scripts/fix-wayland.sh --check      # 只看状态不动手
#
# 幂等：已经是 1.24 就只做验证。

set -uo pipefail

new_ver="1.24.0-1build1"
newup="1.24.0"
pool_wl="http://archive.ubuntu.com/ubuntu/pool/main/w/wayland"
pool_lx="http://archive.ubuntu.com/ubuntu/pool/main/libx/libxml2"
new_dir="/home/retro/wayland-1.24"
rollback_root="/home/retro/wayland-rollback"
xml2_deb="libxml2-16_2.14.5+dfsg-0.2_amd64.deb"
xml2_sha="89816afcd0d557305a9e126ed63a104bbfd6afc80a426c87fbad89445973e266"

check_only=0
[[ "${1:-}" == "--check" ]] && check_only=1

run_sudo() { if [[ "${EUID}" -eq 0 ]]; then "$@"; else sudo "$@"; fi; }

# 必须 root：容器里的 retro 没有 sudo，dpkg 装包只能 root
if [[ "${EUID}" -ne 0 ]] && ! command -v sudo >/dev/null && [[ "${check_only}" -eq 0 ]]; then
    echo "需要 root（或 sudo）：用 docker exec -u root 跑本脚本" >&2
    exit 1
fi

# 1.24 的 13 个包（amd64 + i386）
new_debs=(
    libwayland-client0_${new_ver}_amd64.deb   libwayland-client0_${new_ver}_i386.deb
    libwayland-server0_${new_ver}_amd64.deb   libwayland-server0_${new_ver}_i386.deb
    libwayland-egl1_${new_ver}_amd64.deb      libwayland-egl1_${new_ver}_i386.deb
    libwayland-cursor0_${new_ver}_amd64.deb   libwayland-cursor0_${new_ver}_i386.deb
    libwayland-bin_${new_ver}_amd64.deb
    libwayland-dev_${new_ver}_amd64.deb
)
declare -A sha=(
    [libwayland-client0_${new_ver}_amd64.deb]=50183cb2bade7a6018311973ef246f320941fc56a5348eb259461313d9b03495
    [libwayland-client0_${new_ver}_i386.deb]=4293c542814ae99cf569ca3a5595307bff194d5b670418dbdb64819104cee2d1
    [libwayland-server0_${new_ver}_amd64.deb]=2052a7a0829c54e411e57daa5f4c584a38e34c4f33f2d4684c4fd710de48978e
    [libwayland-server0_${new_ver}_i386.deb]=c267939d252f243d3992460c58dcd7e9ffe27c4294be6b00c3294b4be0bdaffe
    [libwayland-egl1_${new_ver}_amd64.deb]=e60dc4b6abda3e90622ce7d5afb7c439e29ff1532a2eb7583e49e3d2ad3af407
    [libwayland-egl1_${new_ver}_i386.deb]=d6407dbc1a78de2c92a917d82cfcdf193d588fb8197c1d916d3279218a429035
    [libwayland-cursor0_${new_ver}_amd64.deb]=fd73eb9f9ae457cdcc93762d88d53a065ffdf1fd8b102c822a9139970b97ff30
    [libwayland-cursor0_${new_ver}_i386.deb]=d8e63c1a50c81f41751429443aebaf6a72df98c5f0b3505d3aa5a49c02a2699f
    [libwayland-bin_${new_ver}_amd64.deb]=a0607eb15dff01fc42e5a7a0bbe257b1f459c109e71091ae99ccec5653732867
    [libwayland-dev_${new_ver}_amd64.deb]=15e66f22b90e712588d06c5f9a26abec09512152315d6eed14e488b2163f5b09
)

cur="$(dpkg-query -W -f='${Version}' libwayland-client0:amd64 2>/dev/null || echo none)"
echo "== 当前 libwayland-client0: ${cur} =="
sym_ok=0
if nm -D /usr/lib/x86_64-linux-gnu/libwayland-client.so.0 2>/dev/null | grep -q 'wl_display_dispatch_queue_timeout'; then
    sym_ok=1; echo "   wl_display_dispatch_queue_timeout: 存在 ✓"
else
    echo "   wl_display_dispatch_queue_timeout: **缺失**（Wayland 显示会 symbol lookup error）"
fi
if [[ "${cur}" == "${new_ver}" && "${sym_ok}" -eq 1 ]]; then
    echo "   已经修好，无需再动。"
    [[ "${check_only}" -eq 1 ]] && exit 0
fi
[[ "${check_only}" -eq 1 ]] && exit 0

# ---------------------------------------------------------------- 备份旧包
old_dir="${rollback_root}/${cur}"
mkdir -p "${old_dir}" "${new_dir}"
if ! ls "${old_dir}"/*.deb >/dev/null 2>&1; then
    echo "== 备份当前版本（${cur}）到 ${old_dir} =="
    ( cd "${old_dir}" || exit 1
      for p in libwayland-client0 libwayland-server0 libwayland-egl1 libwayland-cursor0 libwayland-bin libwayland-dev; do
          apt-get download "${p}" >/dev/null 2>&1
          apt-get download "${p}:i386" >/dev/null 2>&1
      done
      sha256sum ./*.deb > SHA256SUMS 2>/dev/null
    )
    ls -1 "${old_dir}"/*.deb 2>/dev/null | wc -l | xargs -I{} echo "   备份 {} 个 deb"
fi
dpkg -l | grep -E 'wayland' | awk '{print $2" "$3}' > "${rollback_root}/pre-upgrade-snapshot.txt"
echo "   快照: ${rollback_root}/pre-upgrade-snapshot.txt"

# ---------------------------------------------------------------- 下载新包
echo "== 下载 wayland ${new_up}（questing 池） =="
( cd "${new_dir}" || exit 1
  for f in "${new_debs[@]}"; do
      [[ -f "${f}" ]] && continue
      curl -fsSL -O "${pool_wl}/${f}" || echo "   **下载失败**: ${f}" >&2
  done
  if [[ ! -f "${xml2_deb}" ]]; then
      curl -fsSL -O "${pool_lx}/${xml2_deb}" || echo "   **下载失败**: ${xml2_deb}" >&2
  fi
  echo "== 校验 SHA256 =="
  fail=0
  for f in "${new_debs[@]}"; do
      [[ -f "${f}" ]] || { echo "   缺文件 ${f}"; fail=1; continue; }
      echo "${sha[$f]}  ${f}" | sha256sum -c --quiet - || { echo "   **SHA 不匹配**: ${f}"; fail=1; }
  done
  echo "${xml2_sha}  ${xml2_deb}" | sha256sum -c --quiet - || { echo "   **SHA 不匹配**: ${xml2_deb}"; fail=1; }
  [[ "${fail}" -eq 0 ]] && echo "   全部匹配 ✓"
  sha256sum ./*.deb > SHA256SUMS.txt
  [[ "${fail}" -eq 0 ]] || exit 1
) || { echo "下载/校验失败，中止（未改动系统）" >&2; exit 1; }

# ---------------------------------------------------------------- 安装
echo "== dpkg -i（先 libxml2-16，再 wayland 全家） =="
run_sudo dpkg -i "${new_dir}/${xml2_deb}" || true
( cd "${new_dir}" && run_sudo dpkg -i "${new_debs[@]}" ) || true
run_sudo dpkg --configure -a || true
run_sudo ldconfig

# ---------------------------------------------------------------- 验证
echo "== 验证 =="
dpkg -l | grep -E 'wayland' | awk '{print "   "$1" "$2" "$3}'
if nm -D /usr/lib/x86_64-linux-gnu/libwayland-client.so.0 | grep -q 'wl_display_dispatch_queue_timeout'; then
    echo "   wl_display_dispatch_queue_timeout: 存在 ✓"
else
    echo "   **符号仍缺失**" >&2
fi
if apt-get check >/dev/null 2>&1; then
    echo "   apt-get check: 通过 ✓"
else
    echo "   **apt-get check 有未满足依赖**（跑 apt-get check 看详情）" >&2
fi
cat <<EOF

回退到 ${cur}：
  cd ${old_dir} && dpkg -i ./*.deb && ldconfig

验证显示：
  XDG_RUNTIME_DIR=/run/user/wolf WAYLAND_DISPLAY=wayland-2 vulkaninfo --summary | grep deviceName
  XDG_RUNTIME_DIR=/run/user/wolf WAYLAND_DISPLAY=wayland-2 \\
    mpv --no-config --no-audio --vo=gpu-next --gpu-api=vulkan --frames=30 \\
        --vf="vapoursynth=/home/retro/dnr/repo/dnr.vpy" /home/retro/dnr/sample.mp4
  # 应看到 "VO: [gpu-next] 1920x1080 yuv444p" 且不再有 symbol lookup error
EOF
