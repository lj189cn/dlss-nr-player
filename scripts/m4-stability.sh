#!/usr/bin/env bash
# m4-stability.sh — 1080p 长时稳定性 + 显存/掉帧采样（真显示路径）
#
# 用法（容器内，建议用 retro 用户）：
#   XDG_RUNTIME_DIR=/run/user/wolf WAYLAND_DISPLAY=wayland-2 \
#   DNR_ROOT=/home/retro/dlssnr-test DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so \
#   bash repo/scripts/m4-stability.sh [--sample FILE] [--frames N] [--interval S] [--vo gpu-next|null]
#
# 默认：sample_10min.mp4（18000 帧 / 600 s）、每 30 s 采样一次、真显示（gpu-next + vulkan）。
# 采样内容：VRAM（/sys/class/drm/cardN/device/mem_info_vram_used）、mpv RSS、
#           estimated-vf-fps、frame-drop-count、decoder-frame-drop-count、time-pos。
# 结束后统计插件 "（100 帧）" 统计条数（= 实际进内核的帧数）与退出码。

set -uo pipefail

sample="${SAMPLE:-/home/retro/dnr/sample_10min.mp4}"
frames="${FRAMES:-18000}"
interval="${INTERVAL:-30}"
vo="${VO:-gpu-next}"
gpu_api="vulkan"
out_dir="${OUT_DIR:-/tmp}"
sock="/tmp/mpv-m4s.sock"
log="${out_dir}/m4_stability_monitor.log"
mpvlog="${out_dir}/m4_stability_mpv.log"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sample)   sample="$2"; shift 2 ;;
        --frames)   frames="$2"; shift 2 ;;
        --interval) interval="$2"; shift 2 ;;
        --vo)       vo="$2"; shift 2 ;;
        -h|--help)  sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

: "${XDG_RUNTIME_DIR:=/run/user/wolf}"
: "${WAYLAND_DISPLAY:=wayland-2}"
: "${HOME:=/home/retro}"
export XDG_RUNTIME_DIR WAYLAND_DISPLAY HOME
: "${DNR_ROOT:=/home/retro/dlssnr-test}"
: "${DNR_PLUGIN:=/usr/local/lib/vapoursynth/dnr.so}"
export DNR_ROOT DNR_PLUGIN

vram_file="$(ls -1 /sys/class/drm/card*/device/mem_info_vram_used 2>/dev/null | head -1)"
vram_mib() { awk -v b="$(cat "${vram_file:-/dev/zero}" 2>/dev/null || echo 0)" 'BEGIN{printf "%d", b/1048576}'; }

rm -f "${sock}" "${log}" "${mpvlog}"
: >>"${log}"
base_vram=$(vram_mib)
{
    echo "start $(date -Is)"
    echo "  sample=${sample} frames=${frames} interval=${interval}s vo=${vo} gpu_api=${gpu_api}"
    echo "  vram_file=${vram_file} base_vram=${base_vram}MiB"
} >>"${log}"

extra=()
[[ "${vo}" != "null" ]] && extra+=(--vo="${vo}" --gpu-api="${gpu_api}")

mpv --no-config --no-audio --ao=null "${extra[@]}" --frames="${frames}" \
    --input-ipc-server="${sock}" \
    --vf="vapoursynth=/home/retro/dnr/repo/dnr.vpy" "${sample}" >"${mpvlog}" 2>&1 &
mpvpid=$!
sleep 5

samples=0
while kill -0 "${mpvpid}" 2>/dev/null; do
    sleep "${interval}"
    kill -0 "${mpvpid}" 2>/dev/null || break
    rss=$(ps -o rss= -p "${mpvpid}" 2>/dev/null | tr -d ' ' || echo 0)
    stats=$(python3 - <<'PY' 2>/dev/null
import json, socket, time
try:
    s = socket.socket(socket.AF_UNIX); s.settimeout(2); s.connect("/tmp/mpv-m4s.sock")
    def q(p):
        s.send((json.dumps({"command": ["get_property", p]}) + "\n").encode())
        time.sleep(0.05)
        return json.loads(s.recv(65536).decode().strip().splitlines()[-1]).get("data")
    print("pos=%s vf_fps=%s drop=%s dec_drop=%s" % (
        q("time-pos"), q("estimated-vf-fps"), q("frame-drop-count"), q("decoder-frame-drop-count")))
    s.close()
except Exception as e:
    print("ipc_err=%s" % e)
PY
)
    echo "$(date +%H:%M:%S) vram=$(vram_mib)MiB rss=$(( ${rss:-0} / 1024 ))MiB ${stats}" >>"${log}"
    samples=$((samples + 1))
done

wait "${mpvpid}"; rc=$?
{
    echo "end $(date -Is) rc=${rc} vram=$(vram_mib)MiB samples=${samples}"
    echo "  插件统计条数=$(grep -c '平均 network' "${mpvlog}")（×100 帧）"
    echo "  最后一条=$(grep -oE '平均 network [0-9.]+ ms' "${mpvlog}" | tail -1)"
    grep -iE "error|crash|segmentation|assert" "${mpvlog}" | tail -5
} >>"${log}"
echo "== 完成，日志 ${log} =="
tail -6 "${log}"
