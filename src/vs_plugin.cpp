/*
 * vs_plugin.cpp — VapourSynth 4 插件：core.dnr.Enhance(clip, intensity, style, temporal)
 *
 * 设计要点（与 产出/04_VapourSynth与mpv集成.md 对应）：
 *   1. 插件本身**不链接** libvapoursynth / libvsscript：API 4 的插件通过
 *      VapourSynthPluginInit2 拿到 VSPLUGINAPI，只需要头文件即可编译。
 *      → 只依赖本仓库 third_party/vapoursynth/include 下的头文件。
 *   2. 真正干活的是 libdnr 的 C API（src/dnr_c.h）：
 *        dnr_create / dnr_process_rgba / dnr_destroy
 *      没有 libdnr 时用 src/dnr_stub.cpp 直通（集成调试用）。
 *   3. 懒初始化：第一次 getFrame(arInitial) 才 dnr_create（上传 ~141MiB 权重，数秒）；
 *      全局单例 + mutex，多个 filter 实例共享同一个 Vulkan 内核。
 *   4. 处理失败 → 回退原帧 + fprintf(stderr) + VS 日志；连续失败过多则整体透传。
 *   5. 输入 RGB24（8bit）或 RGBS（float32，按 sRGB 编码解释）；输出恒为同尺寸 RGB24。
 *   6. temporal 时序：fmParallelRequests 保证 arAllFramesReady 串行，但帧可能乱序到达；
 *      帧号不连续（seek/跳帧）时调用 dnr_reset_history()，避免历史污染。
 *   7. A/B：环境变量 DNR_BYPASS_FILE 指向的“开关文件”存在时直接透传；
 *      DNR_INTENSITY_FILE 可在播放中改强度。均只做一次 stat()/小文件读取。
 *
 * 线程模型：fmParallelRequests → 同一时刻只有一个线程在 arAllFramesReady；
 * 额外的 mutex 只用于保护跨实例（跨 core）共享的全局内核。
 */

#include "VapourSynth4.h"

#if !defined(VAPOURSYNTH_API_MAJOR) || VAPOURSYNTH_API_MAJOR != 4
#  error "dnr VS plugin 需要 VapourSynth API 4 (R4x-R8x) 的头文件 VapourSynth4.h"
#endif

#include "VSHelper4.h"

#include "dnr_c.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  define DNR_FILE_EXISTS(p) (_access((p), 0) == 0)
#else
#  include <unistd.h>
#  define DNR_FILE_EXISTS(p) (access((p), F_OK) == 0)
#endif

namespace {

constexpr const char *kPluginId   = "dev.dnr.enhance";
constexpr const char *kNamespace  = "dnr";
constexpr const char *kPluginName = "DLSSNR-AMD native enhancer (RDNA4)";
constexpr int kMaxConsecutiveFailures = 8;
constexpr int kLogEveryFrames = 100;

/* ------------------------------------------------------------------ */
/* 全局内核（单例）                                                     */
/* ------------------------------------------------------------------ */

struct GlobalSession {
    std::mutex mu;
    dnr_context *ctx = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    int temporal = 0;
    bool attempted = false;
    bool failed = false;
    std::string error;
    int64_t expected_next = -1; /* 期望的下一帧号；用于发现乱序/seek */

    ~GlobalSession() {
        if (ctx) {
            dnr_destroy(ctx);
            ctx = nullptr;
        }
    }
};

GlobalSession &global() {
    static GlobalSession s;
    return s;
}

void vs_log(const VSAPI *vsapi, VSCore *core, int msg_type, const std::string &msg) {
    if (vsapi && core)
        vsapi->logMessage(msg_type, msg.c_str(), core);
}

/* ------------------------------------------------------------------ */
/* 每个 filter 实例的数据                                               */
/* ------------------------------------------------------------------ */

struct DnrData {
    VSNode *node = nullptr;
    VSVideoInfo out_vi{};   /* 输出：同尺寸 RGB24 */
    VSVideoFormat out_fmt{};
    float intensity = 1.0f;
    int style = 0;
    int temporal = 1;
    bool input_is_float = false; /* RGBS → 需要转 8bit */

    bool use_bypass_file = false;
    std::string bypass_path;
    bool use_intensity_file = false;
    std::string intensity_path;

    std::vector<uint8_t> in_buf;  /* RGBA8，打包后的输入 */
    std::vector<uint8_t> out_buf; /* RGBA8，内核输出 */

    double acc_ms = 0.0;
    int64_t acc_frames = 0;
    int consecutive_failures = 0;
    bool disabled = false; /* 连续失败后彻底透传 */
};

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

float read_float_file(const std::string &path, float fallback) {
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return fallback;
    char buf[64] = {0};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0)
        return fallback;
    char *end = nullptr;
    const float v = std::strtof(buf, &end);
    if (end == buf || !std::isfinite(v))
        return fallback;
    return std::clamp(v, 0.0f, 2.0f);
}

inline uint8_t float_to_u8(float v) {
    if (!(v > 0.0f)) /* 同时过滤 NaN */
        return 0;
    if (v >= 1.0f)
        return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

/* RGB24 (3 planes, 8bit) → RGBA8 连续缓冲 */
void pack_rgb24(const VSFrame *src, int w, int h, uint8_t *out, const VSAPI *vsapi) {
    const uint8_t *r = vsapi->getReadPtr(src, 0);
    const uint8_t *g = vsapi->getReadPtr(src, 1);
    const uint8_t *b = vsapi->getReadPtr(src, 2);
    const ptrdiff_t rs = vsapi->getStride(src, 0);
    const ptrdiff_t gs = vsapi->getStride(src, 1);
    const ptrdiff_t bs = vsapi->getStride(src, 2);
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = out + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = r[x];
            dst[x * 4 + 1] = g[x];
            dst[x * 4 + 2] = b[x];
            dst[x * 4 + 3] = 255;
        }
        r += rs;
        g += gs;
        b += bs;
    }
}

/* RGBS (3 planes, float32 sRGB 编码 0..1) → RGBA8 */
void pack_rgbs(const VSFrame *src, int w, int h, uint8_t *out, const VSAPI *vsapi) {
    const float *r = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 0));
    const float *g = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 1));
    const float *b = reinterpret_cast<const float *>(vsapi->getReadPtr(src, 2));
    const ptrdiff_t rs = vsapi->getStride(src, 0) / static_cast<ptrdiff_t>(sizeof(float));
    const ptrdiff_t gs = vsapi->getStride(src, 1) / static_cast<ptrdiff_t>(sizeof(float));
    const ptrdiff_t bs = vsapi->getStride(src, 2) / static_cast<ptrdiff_t>(sizeof(float));
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = out + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = float_to_u8(r[x]);
            dst[x * 4 + 1] = float_to_u8(g[x]);
            dst[x * 4 + 2] = float_to_u8(b[x]);
            dst[x * 4 + 3] = 255;
        }
        r += rs;
        g += gs;
        b += bs;
    }
}

/* RGBA8 → RGB24 输出帧（3 planes） */
void unpack_rgb24(VSFrame *dst, int w, int h, const uint8_t *in, const VSAPI *vsapi) {
    uint8_t *r = vsapi->getWritePtr(dst, 0);
    uint8_t *g = vsapi->getWritePtr(dst, 1);
    uint8_t *b = vsapi->getWritePtr(dst, 2);
    const ptrdiff_t rs = vsapi->getStride(dst, 0);
    const ptrdiff_t gs = vsapi->getStride(dst, 1);
    const ptrdiff_t bs = vsapi->getStride(dst, 2);
    for (int y = 0; y < h; ++y) {
        const uint8_t *src = in + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            r[x] = src[x * 4 + 0];
            g[x] = src[x * 4 + 1];
            b[x] = src[x * 4 + 2];
        }
        r += rs;
        g += gs;
        b += bs;
    }
}

/* ------------------------------------------------------------------ */
/* 懒初始化（每帧 arInitial 调用；内部有 mutex + 失败标记）             */
/* ------------------------------------------------------------------ */

bool ensure_core(DnrData *d, const VSAPI *vsapi, VSCore *core) {
    GlobalSession &s = global();
    std::lock_guard<std::mutex> lock(s.mu);

    if (s.failed)
        return false;
    if (s.ctx && s.width == static_cast<uint32_t>(d->out_vi.width) &&
        s.height == static_cast<uint32_t>(d->out_vi.height) && s.temporal == d->temporal)
        return true;

    if (s.ctx) {
        /* 分辨率或 temporal 变化：旧内核尺寸不匹配，重建。 */
        std::string msg = "dnr: 内核尺寸/模式变化，重建（" + std::to_string(s.width) + "x" +
                          std::to_string(s.height) + " temporal=" + std::to_string(s.temporal) +
                          " → " + std::to_string(d->out_vi.width) + "x" +
                          std::to_string(d->out_vi.height) + " temporal=" +
                          std::to_string(d->temporal) + "）";
        std::fprintf(stderr, "[dnr] %s\n", msg.c_str());
        vs_log(vsapi, core, mtWarning, msg);
        dnr_destroy(s.ctx);
        s.ctx = nullptr;
    }

    dnr_config cfg{};
    cfg.root = std::getenv("DNR_ROOT"); /* 允许 NULL：由实现使用默认值 */
    cfg.width = static_cast<uint32_t>(d->out_vi.width);
    cfg.height = static_cast<uint32_t>(d->out_vi.height);
    cfg.temporal = d->temporal;
    cfg.model_scale = 1.0f;
    cfg.gpu_index = -1;
    cfg.verbose = 1;
    cfg.cfg_size = static_cast<uint32_t>(sizeof(cfg));

    if (const char *v = std::getenv("DNR_MODEL_SCALE")) {
        const float f = std::strtof(v, nullptr);
        /* libdnr 的约定：model_scale ∈ (0, 1]，超过 1 会被 dnr_create 拒绝。 */
        if (f > 0.0f && f <= 1.0f)
            cfg.model_scale = f;
    }
    if (const char *v = std::getenv("DNR_GPU")) {
        char *end = nullptr;
        const long idx = std::strtol(v, &end, 10);
        if (end != v)
            cfg.gpu_index = static_cast<int>(idx);
    }
    if (const char *v = std::getenv("DNR_VERBOSE")) {
        char *end = nullptr;
        const long lvl = std::strtol(v, &end, 10);
        if (end != v)
            cfg.verbose = static_cast<int>(lvl);
    }

    s.attempted = true;
    std::fprintf(stderr, "[dnr] 初始化内核 %ux%u temporal=%d（首次需上传权重，可能数秒）…\n",
                 cfg.width, cfg.height, cfg.temporal);

    char errbuf[512] = {0};
    s.ctx = dnr_create(&cfg, errbuf, sizeof(errbuf));
    if (!s.ctx) {
        s.failed = true;
        s.error = (errbuf[0] != '\0') ? errbuf : "dnr_create 失败（无错误信息）";
        std::fprintf(stderr, "[dnr] 初始化失败：%s —— 后续帧直接透传\n", s.error.c_str());
        vs_log(vsapi, core, mtCritical, "dnr: 初始化失败: " + s.error + " —— 直接透传");
        return false;
    }

    s.width = cfg.width;
    s.height = cfg.height;
    s.temporal = cfg.temporal;
    s.expected_next = -1;

    std::fprintf(stderr, "[dnr] 内核就绪：%s\n", dnr_version());
    vs_log(vsapi, core, mtInformation, std::string("dnr: 内核就绪 ") + dnr_version());
    return true;
}

/* ------------------------------------------------------------------ */
/* getFrame                                                           */
/* ------------------------------------------------------------------ */

const VSFrame *VS_CC dnrGetFrame(int n, int activationReason, void *instanceData, void **frameData,
                                 VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;
    DnrData *d = static_cast<DnrData *>(instanceData);

    if (activationReason == arError)
        return nullptr;
    if (activationReason == arInitial) {
        const bool bypassed = d->use_bypass_file && DNR_FILE_EXISTS(d->bypass_path.c_str());
        if (!bypassed && !d->disabled)
            ensure_core(d, vsapi, core); /* 失败已在内部记录并标记透传 */
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady)
        return nullptr;

    const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
    if (!src)
        return nullptr;

    GlobalSession &s = global();
    std::lock_guard<std::mutex> lock(s.mu);

    const bool bypassed = d->use_bypass_file && DNR_FILE_EXISTS(d->bypass_path.c_str());
    if (bypassed || d->disabled || s.failed || !s.ctx) {
        return src; /* 原帧透传；返回即转移引用，不能 freeFrame */
    }

    const int w = d->out_vi.width;
    const int h = d->out_vi.height;
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (d->in_buf.size() != pixels * 4 || d->out_buf.size() != pixels * 4) {
        try {
            d->in_buf.resize(pixels * 4);
            d->out_buf.resize(pixels * 4);
        } catch (const std::bad_alloc &) {
            std::fprintf(stderr, "[dnr] 帧缓冲分配失败（%dx%d）—— 透传\n", w, h);
            return src;
        }
    }

    /* 1) 打包成 RGBA8 */
    if (d->input_is_float)
        pack_rgbs(src, w, h, d->in_buf.data(), vsapi);
    else
        pack_rgb24(src, w, h, d->in_buf.data(), vsapi);

    /* 2) 参数 */
    dnr_controls ctrl{};
    ctrl.intensity = d->intensity;
    ctrl.style = d->style;
    ctrl.reset_history = 0;
    ctrl.ctrl_size = static_cast<uint32_t>(sizeof(ctrl));

    if (d->use_intensity_file) {
        const float live = read_float_file(d->intensity_path, d->intensity);
        if (live != d->intensity)
            d->intensity = std::clamp(live, 0.0f, 2.0f);
        ctrl.intensity = d->intensity;
    }

    if (d->temporal && s.expected_next != static_cast<int64_t>(n))
        ctrl.reset_history = 1; /* seek / 帧序跳变：丢弃 temporal 历史 */

    /* 3) 调用内核（同步；返回时 dst 已可用） */
    float net_ms = -1.0f;
    const int rc = dnr_process_rgba(s.ctx, d->in_buf.data(), d->out_buf.data(), &ctrl, &net_ms);
    s.expected_next = static_cast<int64_t>(n) + 1;

    if (rc != DNR_OK) {
        ++d->consecutive_failures;
        const char *detail = dnr_last_error(s.ctx);
        std::fprintf(stderr,
                     "[dnr] frame %d 处理失败：%s (%d) — %s；回退原帧%s\n",
                     n, dnr_status_string(rc), rc, detail ? detail : "(no detail)",
                     (d->consecutive_failures >= kMaxConsecutiveFailures) ? "，并停止后续处理" : "");
        vs_log(vsapi, core, mtWarning,
               std::string("dnr: frame ") + std::to_string(n) + " 处理失败 " +
                   dnr_status_string(rc) + "，回退原帧");
        if (d->consecutive_failures >= kMaxConsecutiveFailures) {
            d->disabled = true;
            vs_log(vsapi, core, mtCritical, "dnr: 连续失败过多，本实例后续帧全部透传");
        }
        return src;
    }
    d->consecutive_failures = 0;

    /* 4) 输出帧（props 从 src 复制） */
    VSFrame *dst = vsapi->newVideoFrame(&d->out_fmt, w, h, src, core);
    if (!dst) {
        std::fprintf(stderr, "[dnr] newVideoFrame 失败（%dx%d）—— 透传\n", w, h);
        return src;
    }
    unpack_rgb24(dst, w, h, d->out_buf.data(), vsapi);
    vsapi->freeFrame(src);

    /* 5) 统计 */
    if (net_ms > 0.0f) {
        d->acc_ms += net_ms;
        ++d->acc_frames;
        if (d->acc_frames % kLogEveryFrames == 0) {
            const double avg = d->acc_ms / static_cast<double>(d->acc_frames);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "dnr: 平均 network %.2f ms（%lld 帧）", avg,
                          static_cast<long long>(d->acc_frames));
            vs_log(vsapi, core, mtDebug, buf);
            std::fprintf(stderr, "[dnr] %s\n", buf);
            d->acc_ms = 0.0;
            d->acc_frames = 0;
        }
    }

    return dst;
}

void VS_CC dnrFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;
    DnrData *d = static_cast<DnrData *>(instanceData);
    if (!d)
        return;
    if (d->node)
        vsapi->freeNode(d->node);
    delete d;
}

/* ------------------------------------------------------------------ */
/* 构造：参数校验 + 创建 filter                                        */
/* ------------------------------------------------------------------ */

void VS_CC dnrCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;
    int err = 0;

    VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    if (!node) {
        vsapi->mapSetError(out, "dnr.Enhance: 缺少必填参数 clip");
        return;
    }

    const VSVideoInfo *vi = vsapi->getVideoInfo(node);
    if (!vi || !vsh::isConstantVideoFormat(vi)) {
        vsapi->mapSetError(out, "dnr.Enhance: 需要恒定格式/尺寸的输入 clip");
        vsapi->freeNode(node);
        return;
    }
    if (vi->width <= 0 || vi->height <= 0) {
        vsapi->mapSetError(out, "dnr.Enhance: 输入尺寸无效");
        vsapi->freeNode(node);
        return;
    }

    const VSVideoFormat &fmt = vi->format;
    const bool is_rgb24 = fmt.colorFamily == cfRGB && fmt.numPlanes == 3 && fmt.subSamplingW == 0 &&
                          fmt.subSamplingH == 0 && fmt.sampleType == stInteger &&
                          fmt.bitsPerSample == 8 && fmt.bytesPerSample == 1;
    const bool is_rgbs = fmt.colorFamily == cfRGB && fmt.numPlanes == 3 && fmt.subSamplingW == 0 &&
                         fmt.subSamplingH == 0 && fmt.sampleType == stFloat &&
                         fmt.bitsPerSample == 32 && fmt.bytesPerSample == 4;
    if (!is_rgb24 && !is_rgbs) {
        vsapi->mapSetError(out,
                           "dnr.Enhance: 初版只支持 RGB24 或 RGBS（无子采样）输入，"
                           "请先用 core.resize.Bicubic(clip, format=vs.RGB24) 转换");
        vsapi->freeNode(node);
        return;
    }

    float intensity = vsapi->mapGetFloatSaturated(in, "intensity", 0, &err);
    if (err)
        intensity = 1.0f;
    if (!std::isfinite(intensity))
        intensity = 1.0f;
    intensity = std::clamp(intensity, 0.0f, 2.0f);

    int style = vsapi->mapGetIntSaturated(in, "style", 0, &err);
    if (err)
        style = 0;
    if (style < 0) {
        vsapi->mapSetError(out, "dnr.Enhance: style 必须 >= 0");
        vsapi->freeNode(node);
        return;
    }

    int temporal = vsapi->mapGetIntSaturated(in, "temporal", 0, &err);
    if (err)
        temporal = 1;
    if (temporal != 0 && temporal != 1) {
        vsapi->mapSetError(out, "dnr.Enhance: temporal 只能是 0 或 1");
        vsapi->freeNode(node);
        return;
    }

    VSVideoFormat out_fmt;
    if (!vsapi->getVideoFormatByID(&out_fmt, pfRGB24, core)) {
        vsapi->mapSetError(out, "dnr.Enhance: 无法取得 RGB24 输出格式");
        vsapi->freeNode(node);
        return;
    }

    DnrData *d = new (std::nothrow) DnrData();
    if (!d) {
        vsapi->mapSetError(out, "dnr.Enhance: 内存不足");
        vsapi->freeNode(node);
        return;
    }
    d->node = node;
    d->out_fmt = out_fmt;
    d->out_vi = *vi;
    d->out_vi.format = out_fmt;
    d->intensity = intensity;
    d->style = style;
    d->temporal = temporal;
    d->input_is_float = is_rgbs;

    if (const char *p = std::getenv("DNR_BYPASS_FILE")) {
        if (*p) {
            d->use_bypass_file = true;
            d->bypass_path = p;
        }
    }
    if (const char *p = std::getenv("DNR_INTENSITY_FILE")) {
        if (*p) {
            d->use_intensity_file = true;
            d->intensity_path = p;
        }
    }

    try {
        d->in_buf.resize(static_cast<size_t>(vi->width) * vi->height * 4);
        d->out_buf.resize(static_cast<size_t>(vi->width) * vi->height * 4);
    } catch (const std::bad_alloc &) {
        vsapi->mapSetError(out, "dnr.Enhance: 帧缓冲分配失败");
        delete d;
        vsapi->freeNode(node);
        return;
    }

    VSFilterDependency deps[] = {{node, rpStrictSpatial}};
    VSNode *out_node = vsapi->createVideoFilter2("Enhance", &d->out_vi, dnrGetFrame, dnrFree,
                                                 fmParallelRequests, deps, 1, d, core);
    if (!out_node) {
        vsapi->mapSetError(out, "dnr.Enhance: createVideoFilter2 失败（输出格式无效？）");
        delete d;
        vsapi->freeNode(node);
        return;
    }
    if (vsapi->mapSetNode(out, "clip", out_node, maReplace) != 0) {
        vsapi->mapSetError(out, "dnr.Enhance: 无法写入输出 map");
        /* freeNode 会触发 dnrFree()：它会 delete d 并释放 d->node，
         * 因此这里不能再手动 delete/freeNode，否则就是 double free。 */
        vsapi->freeNode(out_node);
        return;
    }
    vsapi->freeNode(out_node); /* mapSetNode 已持有引用 */

    vs_log(vsapi, core, mtInformation,
           std::string("dnr: 已创建 Enhance 节点（") + std::to_string(vi->width) + "x" +
               std::to_string(vi->height) + ", intensity=" + std::to_string(intensity) +
               ", temporal=" + std::to_string(temporal) + "）");
}

} // namespace

/* ------------------------------------------------------------------ */
/* 插件入口                                                            */
/* ------------------------------------------------------------------ */

VS_EXTERNAL_API(void)
VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin(kPluginId, kNamespace, kPluginName, VS_MAKE_VERSION(0, 1),
                         VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Enhance",
                             "clip:vnode;"
                             "intensity:float:opt;"
                             "style:int:opt;"
                             "temporal:int:opt;",
                             "clip:vnode;", dnrCreate, nullptr, plugin);
}
