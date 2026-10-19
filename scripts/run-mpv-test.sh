#!/usr/bin/env bash
# run-mpv-test.sh — 在 WolfLutris 里用 mpv + VapourSynth 跑 dnr 插件的可复现测试
#
# 前置（由 build-vapoursynth-mpv.sh 完成）：
#   · build-vapoursynth-mpv.sh --prefix /usr/local           # VS R81 + mpv 0.41
#   · build-vs-plugin.sh --library <libdnr.so> --prefix /usr/local
#   · libdnr.so 的 RUNPATH 用 LD_LIBRARY_PATH 兜底（见下）
#
# 用法：
#   scripts/run-mpv-test.sh                        # 自检 + 300 帧基准 + 截图，全部跑
#   scripts/run-mpv-test.sh --sample X.mp4 --frames 300 --out /tmp/m3
#   scripts/run-mpv-test.sh --only bench           # self|bench|rt|shots
#
# 环境：
#   DNR_ROOT    DLSSNR 安装根（默认 /home/retro/dlssnr-test）
#   DNR_PLUGIN  dnr.so 路径（默认 /usr/local/lib/vapoursynth/dnr.so）
#   LIBDNR      libdnr.so 路径（默认 /home/retro/dnr/build/libdnr.so）
#   MPV         mpv 可执行文件（默认 PATH 里的 mpv）
#
# 说明：
#   · 全部用 `--vo=null --ao=null --no-audio --no-config`，不需要显示/声卡
#   · 插件每 100 帧往 stderr 打一行 `[dnr] dnr: 平均 network xx ms（N 帧）`
#     → 这就是"每帧确实进了内核"的证据（3 行 = 300 帧）
#   · 首次初始化要上传 141 MiB 权重：pipeline cache 命中约 0.8 s，无缓存 20+ s

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

sample="${SAMPLE:-/home/retro/dnr/sample_600.mp4}"
frames="300"
out_dir="${OUT_DIR:-/tmp/dnr-mpv-test}"
only="all"
dnr_root="${DNR_ROOT:-/home/retro/dlssnr-test}"
libdnr="${LIBDNR:-/home/retro/dnr/build/libdnr.so}"
mpv_bin="${MPV:-mpv}"
vpy="${VPY:-${repo_dir}/dnr.vpy}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sample)  sample="$2"; shift 2 ;;
        --frames)  frames="$2"; shift 2 ;;
        --out)     out_dir="$2"; shift 2 ;;
        --only)    only="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "${out_dir}"
log="${out_dir}/run.log"
: > "${log}"
say() { echo "$*" | tee -a "${log}"; }
kv()  { printf '%-28s %s\n' "$1" "$2" | tee -a "${log}"; }

# ------------------------------------------------------------------ 定位 VS
purelib="$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')"
vs_dir="${purelib}/vapoursynth"
[[ -d "${vs_dir}" ]] || vs_dir="/usr/local/lib/python3/dist-packages/vapoursynth"
export VSSCRIPT_PATH="${VSSCRIPT_PATH:-${vs_dir}/libvsscript.so}"
export DNR_ROOT="${dnr_root}"
export DNR_PLUGIN="${DNR_PLUGIN:-/usr/local/lib/vapoursynth/dnr.so}"
export LD_LIBRARY_PATH="$(dirname "${libdnr}")${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
# 插件在 <site-packages>/vapoursynth/plugins 下才会 autoload；否则 dnr.vpy 用 DNR_PLUGIN 加载
export PYTHONPATH="${vs_dir}/..${PYTHONPATH:+:${PYTHONPATH}}"

common=(--no-config --no-audio --ao=null --vo=null --msg-level=all=info --terminal=yes)

# ------------------------------------------------------------------ 1. 自检
if [[ "${only}" == "all" || "${only}" == "self" ]]; then
    echo "== 1. 自检 ==" | tee -a "${log}"
    "${mpv_bin}" --version 2>&1 | head -1 | tee -a "${log}"
    # 注意：脚本开了 pipefail，`mpv | grep -q` 会因为 grep 提前退出导致 SIGPIPE 判成失败，
    # 所以先落变量再 grep。
    vf_help="$("${mpv_bin}" --vf=help 2>&1)"
    if grep -qi vapoursynth <<<"${vf_help}"; then
        say "   mpv --vf=vapoursynth : OK"
    else
        say "   mpv --vf=vapoursynth : **缺失**（mpv 没编 VS 支持）"
    fi
    kv "VSSCRIPT_PATH" "${VSSCRIPT_PATH}"
    kv "DNR_PLUGIN" "${DNR_PLUGIN}"
    kv "DNR_ROOT" "${DNR_ROOT}"
    kv "dnr.vpy" "${vpy}"
    python3 - <<PY 2>&1 | tee -a "${log}"
import vapoursynth as vs
print("   vapoursynth core:", vs.core)
core = vs.core
core.std.LoadPlugin(path="${DNR_PLUGIN}")
print("   plugin namespaces:", [p.namespace for p in core.plugins() if p.namespace == "dnr"])
PY
    echo | tee -a "${log}"
fi

# ------------------------------------------------------------------ 2. 吞吐基准
# --untimed：不看时钟，能跑多快跑多快 → 量的是"解码+NR+空输出"的上限

# 坑：片源正好 300 帧时，mpv `--frames=300` 的片尾边界会让插件只收到 299 帧，
# 于是只看到 2 条 "（100 帧）" 统计而不是 3 条（2026-10-19 实测）。这里自动用
# -stream_loop 造一个更长的副本，保证跑满 N 帧、统计条数 = N/100。
ensure_long_sample() {
    local n loops looped
    n=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "${sample}" 2>/dev/null | head -1 | tr -dc '0-9')
    [[ -n "${n}" ]] || return 0
    if (( n < frames + 5 )); then
        loops=$(( frames / n + 1 ))
        looped="${out_dir}/looped.mp4"
        ffmpeg -v error -y -stream_loop "${loops}" -i "${sample}" -c copy "${looped}" 2>/dev/null \
            && sample="${looped}" && say "   片源只有 ${n} 帧 → 用 -stream_loop 生成 ${looped}"
    fi
}
ensure_long_sample

run_bench() { # $1=标签 其余=额外环境变量（NAME=VAL）
    local label="$1"; shift
    local logf="${out_dir}/bench_${label}.log"
    local t0 t1 wall fps
    say "-- $label --"
    t0=$(date +%s.%N)
    env "$@" "${mpv_bin}" "${common[@]}" --untimed --frames="${frames}" \
        --vf="vapoursynth=${vpy}" "${sample}" > "${logf}" 2>&1
    local rc=$?
    t1=$(date +%s.%N)
    wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
    fps=$(awk -v n="${frames}" -v w="$wall" 'BEGIN{printf "%.1f", n/w}')
    local processed
    # 插件每 100 帧打一条 "（100 帧）"，所以 条数×100 = 插件实际处理的帧数
    processed=$(( $(grep -c '平均 network' "${logf}") * 100 ))
    kv "   墙钟" "${wall} s"
    kv "   fps（${frames} 帧）" "${fps}"
    kv "   插件处理帧数（统计条数×100）" "${processed} 帧"
    kv "   平均 network 日志" "$(grep -o '平均 network [0-9.]* ms' "${logf}" | tail -1)"
    kv "   内核就绪行" "$(grep -o '内核就绪.*' "${logf}" | head -1)"
    kv "   mpv 退出码" "${rc}"
    echo | tee -a "${log}"
}

if [[ "${only}" == "all" || "${only}" == "bench" ]]; then
    echo "== 2. 吞吐基准（${frames} 帧，--untimed，--vo=null） ==" | tee -a "${log}"
    run_bench "temporal_on"  DNR_TEMPORAL=1
    run_bench "temporal_off" DNR_TEMPORAL=0
    run_bench "modelscale_075" DNR_TEMPORAL=1 DNR_MODEL_SCALE=0.75
fi

# ------------------------------------------------------------------ 3. 实时播放
# 不带 --untimed：跟 30 fps 的时钟走 10 s，看会不会掉帧/卡死
if [[ "${only}" == "all" || "${only}" == "rt" ]]; then
    echo "== 3. 实时播放（--length=10，30 fps 时钟） ==" | tee -a "${log}"
    rtlog="${out_dir}/realtime.log"
    t0=$(date +%s.%N)
    "${mpv_bin}" "${common[@]}" --length=10 --vf="vapoursynth=${vpy}" "${sample}" \
        > "${rtlog}" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    kv "   墙钟" "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}') s（片长 10 s）"
    kv "   插件处理帧数（统计条数×100）" "$(( $(grep -c '平均 network' "${rtlog}") * 100 )) 帧"
    kv "   mpv 退出码" "${rc}"
    grep -iE "dropped|DS: |VO: |\[ffmpeg\]" "${rtlog}" | tail -3 | tee -a "${log}"
    echo | tee -a "${log}"
fi

# ------------------------------------------------------------------ 4. 画质截图
if [[ "${only}" == "all" || "${only}" == "shots" ]]; then
    echo "== 4. 画质截图（--vo=image，NR on/off 同帧） ==" | tee -a "${log}"
    shots="${out_dir}/shots"
    rm -rf "${shots}"; mkdir -p "${shots}/on" "${shots}/off"
    bypass="${out_dir}/.dnr-bypass"
    rm -f "${bypass}"
    # NR ON
    "${mpv_bin}" "${common[@]}" --vo=image \
        --vo-image-outdir="${shots}/on" --vo-image-format=png \
        --start=4 --frames=3 --vf="vapoursynth=${vpy}" "${sample}" \
        > "${out_dir}/shots_on.log" 2>&1
    kv "   NR on 退出码" "$?"
    # NR OFF（DNR_BYPASS_FILE 存在 = 透传，不初始化内核、不花 GPU 时间）
    touch "${bypass}"
    DNR_BYPASS_FILE="${bypass}" "${mpv_bin}" "${common[@]}" --vo=image \
        --vo-image-outdir="${shots}/off" --vo-image-format=png \
        --start=4 --frames=3 --vf="vapoursynth=${vpy}" "${sample}" \
        > "${out_dir}/shots_off.log" 2>&1
    kv "   NR off 退出码" "$?"
    rm -f "${bypass}"
    ls -1 "${shots}/on" "${shots}/off" | tee -a "${log}"
    # PSNR（on vs off，逐帧对齐）。psnr 滤镜的汇总行是 info 级别，必须 -v info 才看得到。
    for f in "${shots}/on"/*.png; do
        b="$(basename "$f")"
        [[ -f "${shots}/off/${b}" ]] || continue
        psnr=$(ffmpeg -v info -i "${f}" -i "${shots}/off/${b}" -lavfi "psnr" -f null - 2>&1 \
               | grep -o 'average:[0-9.a-z]*' | head -1)
        kv "   PSNR ${b} (on vs off)" "${psnr:-n/a}"
    done
    echo | tee -a "${log}"
fi

echo "== 完成；日志 ${log} ==" | tee -a "${log}"
