# dnr.vpy — VapourSynth 4 脚本：把 core.dnr.Enhance 挂进 mpv
#
# 用法（推荐显式给路径）：
#   DNR_ROOT=/home/retro/dlssnr-test \
#   mpv --vf=vapoursynth="$PWD/dnr.vpy" movie.mkv
#
# 环境变量：
#   DNR_PLUGIN         dnr.so 的完整路径（未 autoload 时用；也可 core.std.LoadPlugin 手动加载）
#   DNR_ROOT           DLSSNR-AMD 安装根（含 dlssnr-amd/dlssnr.bin 与 shaders/）
#   DNR_INTENSITY      0.0-2.0，默认 1.0
#   DNR_STYLE          默认 0（预留）
#   DNR_TEMPORAL       1/0，默认 1（视频用 1；单帧/逐帧独立用 0）
#   DNR_MATRIX         YUV<->RGB 用的矩阵，默认 1（BT.709）；SD/601 用 5，BT.2020 用 9。
#                      **必须显式给**：VS R81 下省略 matrix_in，mpv 会报 resize 3074 后禁用滤镜
#   DNR_MATRIX_IN      单独覆盖输入矩阵（不设 = DNR_MATRIX）
#   DNR_MATRIX_OUT     单独覆盖输出矩阵（不设 = DNR_MATRIX；两者不一致会整体偏色）
#   DNR_RANGE_IN       强制输入 range：0=limited, 1=full；不设则用帧属性 _Range
#   DNR_RANGE_OUT      强制输出 range：0=limited（默认）, 1=full
#   DNR_OUT_FORMAT     交给 mpv 的出口像素格式：yuv444p8（默认）/ yuv420p8 / yuv422p8。
#                      **mpv 的 --vf=vapoursynth 只接受 YUV**，RGB 出口会被 mpv 禁用（见文件末尾）
#   DNR_CHECK_TRANSFER 1 = 初始化时取一帧查 HDR（**mpv 下不要开**，会 "Frame requested during init"）
#   DNR_BYPASS_FILE    A/B 开关文件路径（文件存在 = 关闭 NR，直接透传）
#   DNR_INTENSITY_FILE 运行时可改强度的文件（每帧读取）
#
# 注意：mpv 的 video_in 一般是 YUV（420P8/10bit 等），本脚本先转 RGB24（sRGB 编码 SDR）。
#       HDR/PQ/HLG 源没有 tonemap，NR 会把线性/PQ 值当成 sRGB 处理，效果不对——
#       请先让 mpv 输出 SDR，或用 SDR 片源。

import os
import sys

import vapoursynth as vs

core = vs.core


def _load_plugin() -> None:
    """优先 autoload；不行就用 DNR_PLUGIN / 常见安装路径显式加载。"""
    if hasattr(core, "dnr"):
        return

    here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
    candidates = [
        os.environ.get("DNR_PLUGIN", ""),
        os.path.expanduser("~/.local/lib/vapoursynth/dnr.so"),
        "/usr/local/lib/vapoursynth/dnr.so",
        os.path.join(here, "build", "dnr.so"),
    ]
    for path in candidates:
        if path and os.path.exists(path):
            core.std.LoadPlugin(path=path)
            return
    raise RuntimeError(
        "找不到 core.dnr 插件：请设置 DNR_PLUGIN=/path/to/dnr.so，"
        "或把 dnr.so 装进 <site-packages>/vapoursynth/plugins/ 或 ~/.local/lib/vapoursynth/"
    )


def _env_bool(name: str, default: str = "1") -> bool:
    return os.environ.get(name, default).strip().lower() not in ("0", "false", "no", "off", "")


_load_plugin()

intensity = float(os.environ.get("DNR_INTENSITY", "1.0"))
style = int(os.environ.get("DNR_STYLE", "0"))
temporal = _env_bool("DNR_TEMPORAL", "1")

clip = video_in

# ---- 输入色彩检查（只提示，不改流程） ---------------------------------
# 坑（2026-10-19 mpv 0.41 实测）：**mpv 的 --vf=vapoursynth 在脚本初始化阶段禁止取帧**。
# 在这里调 clip.get_frame(0) 会得到
#   [vapoursynth] Frame requested during init! This is unsupported.
#   [vapoursynth] Returning black dummy frame with 0 duration.
#   [vapoursynth] Unsupported output format. / could not init VS
# 然后滤镜被 mpv 直接禁用（画面变成直通，日志里 "Disabling filter vapoursynth.00"）。
# 所以默认**不**在这里取帧；HDR 提示改成显式开关（vspipe/离线时安全）。
if _env_bool("DNR_CHECK_TRANSFER", "0"):
    try:
        props = clip.get_frame(0).props
    except Exception:  # 某些宿主仍然不允许取帧
        props = {}
    transfer = int(props.get("_Transfer", 0) or 0) if props else 0
    if transfer in (16, 18):  # PQ / HLG
        print(
            "[dnr.vpy] 警告：输入是 HDR（transfer=%d），没有 tonemap；"
            "NR 需要 sRGB 编码的 SDR 帧。建议先 tonemap 或用 SDR 片源。" % transfer,
            file=sys.stderr,
        )

# ---- 统一到 RGB24 ------------------------------------------------------
# 坑（2026-10-19 VS R81 实测）：**matrix_in 不能省略**。以前靠帧属性 `_Matrix` 自动判断的写法
# 在 mpv 里会炸：
#   [vapoursynth] Filter error at frame 1: Resize error 3074:
#       no path between colorspaces (2/2/2 => 0/2/2). May need to specify additional colorspace parameters.
# 然后 mpv 直接禁用滤镜。所以这里显式给 matrix_in（默认 BT.709 = 1；SD/601 片源用 DNR_MATRIX=5）。
# 入口和出口用同一个矩阵，否则整体偏色。
mat = int(os.environ.get("DNR_MATRIX", "1"))
resize_args = {
    "format": vs.RGB24,
    "matrix_in": int(os.environ.get("DNR_MATRIX_IN", str(mat))),
}
if os.environ.get("DNR_RANGE_IN"):
    resize_args["range_in"] = int(os.environ["DNR_RANGE_IN"])

if clip.format.id in (vs.RGB24, vs.RGBS):
    rgb = clip  # 插件两种都吃；RGBS 由插件内部转 8bit
else:
    rgb = core.resize.Bicubic(clip, **resize_args)

# ---- 调用插件 ----------------------------------------------------------
enhanced = core.dnr.Enhance(rgb, intensity=intensity, style=style, temporal=temporal)

# ---- 出口格式：mpv 的 --vf=vapoursynth 只吃 YUV -------------------------
# 坑（2026-10-19 mpv 0.41 实测，最硬的一条）：mpv 的 mp_from_vs() 只映射 cfYUV
# （video/filter/vf_vapoursynth.c:140-159，循环里只认 YUV 家族）。脚本如果
# set_output() 一个 RGB 节点（插件默认输出 RGB24），mpv 直接：
#   [vapoursynth] Unsupported output format.
#   [vapoursynth] could not init VS
#   Disabling filter vapoursynth.00 because it has failed.
# 于是滤镜被禁用、画面等于没处理（还很难看出原因）。
# → 插件吐出的 RGB24 必须在这里转回 YUV 再交给 mpv。
#   默认 YUV444P8：不丢色度，最接近 RGB 结果；要和 4:2:0 片源对齐（或喂给 x264 更省）
#   用 DNR_OUT_FORMAT=yuv420p8。矩阵/范围走帧属性（插件 newVideoFrame 复制了 src 的
#   _Matrix/_ColorRange），需要强制时用 DNR_MATRIX_OUT / DNR_RANGE_OUT。
_out_formats = {"yuv444p8": vs.YUV444P8, "yuv420p8": vs.YUV420P8, "yuv422p8": vs.YUV422P8}
_out_name = os.environ.get("DNR_OUT_FORMAT", "yuv444p8").strip().lower()
out_fmt = int(_out_formats.get(_out_name, vs.YUV444P8))  # R81: PresetVideoFormat 是 IntEnum，本身就是 format id
if enhanced.format.id != out_fmt:
    # VS 在**构建图**的时候就要定下 YUV↔RGB 的矩阵（不能等帧属性），所以这里必须显式给，
    # 否则报 `Resize error: Matrix must be specified when converting to YUV or GRAY from RGB`。
    # 默认 BT.709（SDR HD 通用）；和上面 YUV→RGB 用的矩阵保持一致，否则整体偏色。
    out_args = {
        "format": out_fmt,
        "matrix": int(os.environ.get("DNR_MATRIX_OUT", os.environ.get("DNR_MATRIX", "1"))),
    }
    if os.environ.get("DNR_RANGE_OUT"):
        out_args["range"] = int(os.environ["DNR_RANGE_OUT"])
    enhanced = core.resize.Bicubic(enhanced, **out_args)

enhanced.set_output()
