#!/usr/bin/env bash
# test-model-3109.sh - M4.5：从 310.9.16 的 nvngx_dlssnr.dll 解出 small（新）模型，装进 dlssnr-amd，
# 做 small vs original 的性能/画质 A/B。可重跑。
#
# 前置：setup-wolf.sh + build-cli.sh（有 build/dnr-video、build/libdnr.so、host/nr_host）；
#       mpv 部分需要 M3 的 VapourSynth 栈（build-vapoursynth-mpv.sh + build-vs-plugin.sh），没有就自动跳过。
#
# 用法：
#   bash 05_2026-10-19_DLSSNR播放器/repo/dnr/scripts/test-model-3109.sh [--frames 300] [--no-mpv]
#
# 约束：04 号对话文件夹只读；不替换 dlssnr.bin（只新增 dlssnr-small.bin）；失败回退。
set -euo pipefail

CONTAINER=${CONTAINER:-WolfLutris_710937727306524487}
WORK=${WORK:-/home/retro/dnr}
DNR_ROOT=${DNR_ROOT:-/home/retro/dlssnr-test}
FRAMES=${FRAMES:-300}
DO_MPV=1
while [ $# -gt 0 ]; do
    case "$1" in
        --frames) FRAMES=$2; shift 2;;
        --no-mpv) DO_MPV=0; shift;;
        *) echo "未知参数 $1" >&2; exit 2;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT=$(cd "$SCRIPT_DIR/../../.." && pwd)
SPIKE=$PROJECT/产出/spike
LOGDIR=${LOGDIR:-$PROJECT/日志}   # 可覆盖，便于试跑不污染正式日志
DLL_SRC=$PROJECT/../04_2026-10-19_找DLSS5-310.9.16/产出/nvngx_dlssnr_310.9.16/nvngx_dlssnr.dll
DLL_SHA=2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f
TS=$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOGDIR" "$SPIKE"

dc()  { docker exec "$CONTAINER" bash -lc "$*"; }
dr()  { docker exec -u retro "$CONTAINER" bash -lc "cd $WORK && $*"; }
say() { echo "== $*"; }

log() { local f="$LOGDIR/m45_${1}_${TS}.log"; shift; "$@" 2>&1 | tee "$f"; }

# 0. 前置
[ -f "$DLL_SRC" ] || { echo "缺少 310.9.16 DLL：$DLL_SRC" >&2; exit 1; }
[ "$(sha256sum "$DLL_SRC" | cut -d' ' -f1)" = "$DLL_SHA" ] || { echo "DLL SHA256 不符" >&2; exit 1; }
dc "test -x $WORK/build/dnr-video" || { echo "缺少 build/dnr-video：先跑 build-cli.sh" >&2; exit 1; }
dc "test -x $WORK/host/nr_host"    || { echo "缺少 host/nr_host：先跑 setup-wolf.sh" >&2; exit 1; }

# 1. DLL 进容器
say "复制 310.9.16 DLL 进容器"
if ! dc "test -f $DNR_ROOT/nvngx_dlssnr_310.9.16.dll && [ \"\$(sha256sum $DNR_ROOT/nvngx_dlssnr_310.9.16.dll | cut -d' ' -f1)\" = $DLL_SHA ]"; then
    docker cp "$DLL_SRC" "$CONTAINER:$DNR_ROOT/nvngx_dlssnr_310.9.16.dll"
else
    say "容器里已有同 SHA256 的 DLL，跳过"
fi
dc "chown retro:retro $DNR_ROOT/nvngx_dlssnr_310.9.16.dll"

# 2. 备份旧模型（不动它）
say "备份 dlssnr.bin -> dlssnr.bin.3108.bak（只在缺失时）"
dc "[ -f $DNR_ROOT/dlssnr-amd/dlssnr.bin.3108.bak ] || cp -p $DNR_ROOT/dlssnr-amd/dlssnr.bin $DNR_ROOT/dlssnr-amd/dlssnr.bin.3108.bak"
dc "sha256sum $DNR_ROOT/dlssnr-amd/dlssnr.bin $DNR_ROOT/dlssnr-amd/dlssnr.bin.3108.bak"

# 3. 解包（extractor 逐 entry 校验；约 65 s）
if ! dc "test -f $DNR_ROOT/model-3109/dlssnr-small.bin"; then
    say "解包（extract_model.sh，约 65 s）"
    dc "mkdir -p $DNR_ROOT/model-3109 && chown retro:retro $DNR_ROOT/model-3109"
    log extract docker exec -u retro "$CONTAINER" bash -lc \
        "time bash $DNR_ROOT/model-tools/extract_model.sh $DNR_ROOT/nvngx_dlssnr_310.9.16.dll $DNR_ROOT/model-3109 && ls -l $DNR_ROOT/model-3109 && sha256sum $DNR_ROOT/model-3109/*.bin"
else
    say "模型已解包，跳过"
fi
dc "ls -l $DNR_ROOT/model-3109; sha256sum $DNR_ROOT/model-3109/*.bin"

# 4. 只装 small；旧 dlssnr.bin 保持原样
say "安装 dlssnr-small.bin（旧 dlssnr.bin 不动）"
dc "cp -p $DNR_ROOT/model-3109/dlssnr-small.bin $DNR_ROOT/dlssnr-amd/ && chown retro:retro $DNR_ROOT/dlssnr-amd/dlssnr-small.bin"
dc "ls -l $DNR_ROOT/dlssnr-amd/"

# 5. CLI A/B（300 帧 testsrc2 1080p）
bench_cli() { # $1=label $2=env
    log "cli_$1" docker exec -u retro "$CONTAINER" bash -lc \
        "cd $WORK && time ( ffmpeg -v error -f lavfi -i 'testsrc2=size=1920x1080:rate=30:duration=10' \
           -pix_fmt rgba -f rawvideo - 2>/dev/null \
           | env $2 ./build/dnr-video --width 1920 --height 1080 --root $DNR_ROOT --bench --frames $FRAMES )"
}
say "CLI：small（默认）"
bench_cli small ""
say "CLI：original（DLSSNR_MODEL=original）"
bench_cli original "DLSSNR_MODEL=original"

# 6. 单帧 PSNR（310.9.16 的输入就是 310.8.0 的 single-frame-inputs，见 NGX-VERIFICATION.md）
say "单帧 PSNR"
log psnr docker exec -u retro "$CONTAINER" bash -lc "
cd $WORK
V=DLSSNR-AMD-main/docs/ngx-verification
A=\$V/310.9.16/single-frame-outputs
B=\$V/310.8.0/single-frame-outputs
[ -f out/in_1920x1080.rgba ] || ffmpeg -y -v error -i \$V/310.8.0/single-frame-inputs/1920x1080.png -f rawvideo -pix_fmt rgba out/in_1920x1080.rgba
./host/nr_host --in out/in_1920x1080.rgba --out out/m45_small_1920x1080.rgba \
  --width 1920 --height 1080 --root $DNR_ROOT --frames 1 --warmup 0 2>&1 | grep -E 'runtime built|frame 0'
cp -p $DNR_ROOT/dlssnr-amd/pipeline.cache.3108 $DNR_ROOT/dlssnr-amd/pipeline.cache 2>/dev/null || true
DLSSNR_MODEL=original ./host/nr_host --in out/in_1920x1080.rgba --out out/m45_original_1920x1080.rgba \
  --width 1920 --height 1080 --root $DNR_ROOT --frames 1 --warmup 0 2>&1 | grep -E 'runtime built|frame 0'
[ -f $DNR_ROOT/dlssnr-amd/pipeline.cache.small ] && cp -p $DNR_ROOT/dlssnr-amd/pipeline.cache.small $DNR_ROOT/dlssnr-amd/pipeline.cache || true
for m in small original; do ffmpeg -y -v error -f rawvideo -pix_fmt rgba -s 1920x1080 -i out/m45_\${m}_1920x1080.rgba out/m45_\${m}_1920x1080.png; done
ffmpeg -y -v error -i \$V/310.8.0/single-frame-inputs/1920x1080.png out/m45_input_1920x1080.png
p() { printf '%-42s ' \"\$1\"; ffmpeg -hide_banner -i \"\$2\" -i \"\$3\" -lavfi '[0:v][1:v]psnr' -f null - 2>&1 | sed -n 's/.*average:\([0-9.]*\).*/\1/p'; }
p 'small    vs 310.9.16/dlssnr-amd' out/m45_small_1920x1080.png    \$A/1920x1080_dlssnr-amd.png
p 'small    vs 310.9.16/nvidia'     out/m45_small_1920x1080.png    \$A/1920x1080_nvidia.png
p 'small    vs 310.8.0/dlssnr-amd'  out/m45_small_1920x1080.png    \$B/1920x1080_dlssnr-amd.png
p 'original vs 310.9.16/dlssnr-amd' out/m45_original_1920x1080.png \$A/1920x1080_dlssnr-amd.png
p 'original vs 310.8.0/dlssnr-amd'  out/m45_original_1920x1080.png \$B/1920x1080_dlssnr-amd.png
p 'small    vs original'            out/m45_small_1920x1080.png    out/m45_original_1920x1080.png
p 'input    vs 310.9.16/dlssnr-amd' out/m45_input_1920x1080.png    \$A/1920x1080_dlssnr-amd.png
p '310.9.16/dlssnr-amd vs nvidia'   \$A/1920x1080_dlssnr-amd.png    \$A/1920x1080_nvidia.png"

# 7. mpv A/B（可选）
if [ "$DO_MPV" = 1 ] && dc "command -v mpv >/dev/null && test -f /usr/local/lib/vapoursynth/dnr.so"; then
    say "mpv：small vs original（run-mpv-test.sh --only bench）"
    docker cp "$SCRIPT_DIR/run-mpv-test.sh" "$CONTAINER:$WORK/repo/scripts/run-mpv-test.sh"
    dc "chown retro:retro $WORK/repo/scripts/run-mpv-test.sh && chmod 755 $WORK/repo/scripts/run-mpv-test.sh"
    log mpv_small docker exec -u retro "$CONTAINER" bash -lc \
        "cd $WORK && DNR_ROOT=$DNR_ROOT LIBDNR=$WORK/build/libdnr.so bash repo/scripts/run-mpv-test.sh --only bench --frames $FRAMES --out /tmp/m45-mpv-small"
    dc "cp -p $DNR_ROOT/dlssnr-amd/pipeline.cache.3108 $DNR_ROOT/dlssnr-amd/pipeline.cache 2>/dev/null || true"
    log mpv_original docker exec -u retro "$CONTAINER" bash -lc \
        "cd $WORK && DLSSNR_MODEL=original DNR_ROOT=$DNR_ROOT LIBDNR=$WORK/build/libdnr.so bash repo/scripts/run-mpv-test.sh --only bench --frames $FRAMES --out /tmp/m45-mpv-orig"
    dc "cp -p $DNR_ROOT/dlssnr-amd/pipeline.cache.small $DNR_ROOT/dlssnr-amd/pipeline.cache 2>/dev/null || true"
else
    say "跳过 mpv（没装 mpv/dnr.so，或 --no-mpv）"
fi

# 8. 对比图回收
say "回收对比图"
dr "ffmpeg -y -v error -i out/m45_input_1920x1080.png -i out/m45_small_1920x1080.png \
    -i DLSSNR-AMD-main/docs/ngx-verification/310.9.16/single-frame-outputs/1920x1080_dlssnr-amd.png \
    -filter_complex '[0:v]crop=640:360:640:360[a];[1:v]crop=640:360:640:360[b];[2:v]crop=640:360:640:360[c];[a][b][c]hstack=3[v]' \
    -map '[v]' out/m45_strip_small.png"
dr "ffmpeg -y -v error -i out/m45_original_1920x1080.png -i out/m45_small_1920x1080.png \
    -i DLSSNR-AMD-main/docs/ngx-verification/310.9.16/single-frame-outputs/1920x1080_dlssnr-amd.png \
    -filter_complex '[0:v]crop=640:360:640:360[a];[1:v]crop=640:360:640:360[b];[2:v]crop=640:360:640:360[c];[a][b][c]hstack=3[v]' \
    -map '[v]' out/m45_strip_models.png"
for f in m45_strip_small.png m45_strip_models.png; do
    docker cp "$CONTAINER:$WORK/out/$f" "$SPIKE/$f"
done

say "完成。small 已安装（dlssnr-amd/dlssnr-small.bin），旧模型保留在 dlssnr.bin / dlssnr.bin.3108.bak"
say "日志：$LOGDIR/m45_*_${TS}.log"
