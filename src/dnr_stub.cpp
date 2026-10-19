/*
 * dnr_stub.cpp — libdnr C ABI 的直通占位实现（不调用任何 GPU）
 *
 * 用途：在没有 libdnr 的机器上也能把 VapourSynth 插件编译、加载、跑通
 * 整条 mpv → VS → plugin 链路（画面不变），从而先验证集成，再换上真核。
 *
 * 启用方式：CMake 找不到 libdnr 时自动启用（DNR_USE_STUB=ON），
 * 或显式 -DDNR_USE_STUB=ON。定义宏 DNR_BUILD_STUB 防止与真实现重复链接。
 */

#ifdef DNR_BUILD_STUB

#include "dnr_c.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

struct dnr_context {
    dnr_config cfg;
    char last_error[256];
    int logged;
};

namespace {

void set_err(struct dnr_context *ctx, const char *msg, char *errbuf, size_t errbuf_size) {
    if (ctx)
        std::snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", msg ? msg : "");
    if (errbuf && errbuf_size) {
        std::snprintf(errbuf, errbuf_size, "%s", msg ? msg : "");
    }
}

void stub_notice_once(struct dnr_context *ctx) {
    if (ctx->logged)
        return;
    ctx->logged = 1;
    std::fprintf(stderr,
                 "[dnr-stub] libdnr 未链接：当前为直通占位实现，画面不会被增强。\n"
                 "[dnr-stub] 用 -DDNR_LIBRARY=/path/to/libdnr.so -DDNR_USE_STUB=OFF 重新构建。\n");
}

} // namespace

extern "C" {

const char *dnr_version(void) {
    return "dnr-stub 0.1.0 (passthrough)";
}

const char *dnr_status_string(int status) {
    switch (status) {
    case DNR_OK: return "ok";
    case DNR_ERR_INVALID_ARG: return "invalid argument";
    case DNR_ERR_DEVICE: return "device error";
    case DNR_ERR_MODEL: return "model/weights error";
    case DNR_ERR_PROCESS: return "process error";
    case DNR_ERR_UNSUPPORTED: return "unsupported";
    case DNR_ERR_OUT_OF_MEMORY: return "out of memory";
    case DNR_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

dnr_context *dnr_create(const dnr_config *cfg, char *errbuf, size_t errbuf_size) {
    if (!cfg || cfg->width == 0 || cfg->height == 0) {
        set_err(nullptr, "dnr_create: null config or zero dimensions", errbuf, errbuf_size);
        return nullptr;
    }
    if (cfg->cfg_size != 0 && cfg->cfg_size < sizeof(dnr_config)) {
        set_err(nullptr, "dnr_create: cfg_size smaller than expected struct", errbuf, errbuf_size);
        return nullptr;
    }

    dnr_context *ctx = new (std::nothrow) dnr_context();
    if (!ctx) {
        set_err(nullptr, "dnr_create: out of memory", errbuf, errbuf_size);
        return nullptr;
    }
    ctx->cfg = *cfg;
    ctx->cfg.root = nullptr; // stub 不持有外部字符串
    ctx->logged = 0;
    ctx->last_error[0] = '\0';
    stub_notice_once(ctx);
    return ctx;
}

int dnr_process_rgba(dnr_context *ctx, const uint8_t *src, uint8_t *dst,
                     const dnr_controls *ctrl, float *out_network_ms) {
    (void)ctrl;
    if (!ctx)
        return DNR_ERR_INVALID_ARG;
    if (!src || !dst)
        return DNR_ERR_INVALID_ARG;
    if (out_network_ms)
        *out_network_ms = 0.0f;

    const size_t bytes = static_cast<size_t>(ctx->cfg.width) * ctx->cfg.height * 4u;
    if (src != dst)
        std::memcpy(dst, src, bytes);
    return DNR_OK;
}

int dnr_reset_history(dnr_context *ctx) {
    return ctx ? DNR_OK : DNR_ERR_INVALID_ARG;
}

void dnr_destroy(dnr_context *ctx) {
    delete ctx;
}

const char *dnr_last_error(const dnr_context *ctx) {
    return ctx ? ctx->last_error : "no context";
}

} // extern "C"

#endif // DNR_BUILD_STUB
