/*
 * dnr_c.h — C ABI for libdnr (DLSSNR-AMD native Linux core)
 *
 * 这是「调用侧契约」。实现由 wolf-builder 的 libdnr 提供（C++/Vulkan，
 * 封装 DLSSNR-AMD 的 nr::Runtime）；本仓库的 VapourSynth 插件、CLI、
 * 以及未来的 VLC filter 都只依赖这个头文件。
 *
 * 约定：
 *   - 像素格式：RGBA8（每通道 8bit，sRGB 编码的 SDR 值，0-255），
 *     行优先、紧排（stride == width*4），总共 width*height*4 字节。
 *   - 尺寸：context 创建时固定；每帧必须与 dnr_config.width/height 一致。
 *     分辨率变化时必须 dnr_destroy + dnr_create。
 *   - 线程：单个 context 不是线程安全的；调用方必须串行调用
 *     dnr_process_rgba（插件内部用 mutex 做了串行化）。
 *   - 所有权：src/dst 由调用方持有；dnr_process_rgba 不同步于 GPU 队列
 *     之外（实现内部应同步完成回读后返回）。
 *
 * ABI 稳定性：只导出本头文件声明的函数；结构体只增不改（新增字段放在末尾，
 * 调用方用 cfg_size/ctrl_size 传大小）。VERSION 递增表示不兼容变更。
 */

#ifndef DNR_C_H
#define DNR_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(DNR_BUILD_SHARED)
#    define DNR_API __declspec(dllexport)
#  else
#    define DNR_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define DNR_API __attribute__((visibility("default")))
#else
#  define DNR_API
#endif

#define DNR_C_API_VERSION 1

/* 状态码。0 成功；负值失败，可用 dnr_status_string() 转文案。 */
typedef enum dnr_status {
    DNR_OK                    = 0,
    DNR_ERR_INVALID_ARG       = -1,   /* 空指针 / 尺寸为 0 / 参数越界 */
    DNR_ERR_DEVICE            = -2,   /* Vulkan 设备创建失败或没有可用 GPU */
    DNR_ERR_MODEL             = -3,   /* 权重 / shader / root 路径不可用 */
    DNR_ERR_PROCESS           = -4,   /* record() 或回读失败 */
    DNR_ERR_UNSUPPORTED       = -5,   /* 当前构建不支持该格式/特性 */
    DNR_ERR_OUT_OF_MEMORY     = -6,
    DNR_ERR_INTERNAL          = -100
} dnr_status;

/* 不透明句柄，由 dnr_create 分配、dnr_destroy 释放。 */
typedef struct dnr_context dnr_context;

/*
 * 创建参数。
 *   root       : DLSSNR-AMD 安装根（含 dlssnr-amd/dlssnr.bin 与 shaders/）。
 *                NULL 时实现可使用环境变量 DNR_ROOT 或内置默认值。
 *   width/height: 输入输出帧尺寸（必须 > 0）。
 *   temporal   : 0 = 单帧 record()；1 = record_temporal()（需要历史缓冲）。
 *   model_scale: 1.0 默认；<1.0 降分辨率换取速度。
 *   gpu_index  : -1 = 自动（按 vendor 0x1002 枚举）；>=0 指定 Vulkan 物理设备索引。
 *   verbose    : 0 安静；1 摘要；>=2 详细（stdout/stderr）。
 *   cfg_size   : sizeof(dnr_config)，用于向后兼容扩展（设为 0 时按当前版本处理）。
 */
typedef struct dnr_config {
    const char *root;
    uint32_t width;
    uint32_t height;
    int temporal;
    float model_scale;
    int gpu_index;
    int verbose;
    uint32_t cfg_size;
} dnr_config;

/*
 * 每帧控制参数。
 *   intensity    : 0.0-2.0，1.0 = 原始网络输出强度。
 *   style        : 预留（0 = 默认权重/风格）。
 *   reset_history: 非 0 时在本次处理前清空 temporal 历史（seek、帧序跳变时用）。
 *   ctrl_size    : sizeof(dnr_controls)（设为 0 时按当前版本处理）。
 */
typedef struct dnr_controls {
    float intensity;
    int style;
    int reset_history;
    uint32_t ctrl_size;
} dnr_controls;

/* 返回库版本字符串（如 "dnr 0.1.0"），永不为 NULL。 */
DNR_API const char *dnr_version(void);

/* 状态码转静态字符串，永不为 NULL。 */
DNR_API const char *dnr_status_string(int status);

/*
 * 创建 context（含 Vulkan 设备、权重上传、shader 加载；首次可能耗时数秒）。
 * 失败返回 NULL，并把原因写进 errbuf（可为 NULL / errbuf_size 为 0）。
 * 注意：errbuf 不保证 NUL 结尾之外的行为，按 strncpy 语义处理。
 */
DNR_API dnr_context *dnr_create(const dnr_config *cfg, char *errbuf, size_t errbuf_size);

/*
 * 处理一帧 RGBA8（sRGB 编码 SDR）。
 *   src/dst  : 各 width*height*4 字节，可指向同一块内存（实现需支持 in-place），
 *              但插件侧始终使用不同缓冲。
 *   ctrl     : 可为 NULL（等同 intensity=1.0, style=0, reset_history=0）。
 *   out_network_ms: 可为 NULL；非 NULL 时写回纯网络耗时（毫秒）。
 * 返回 DNR_OK 或负值错误码；失败时 dst 内容未定义，调用方应回退原帧。
 */
DNR_API int dnr_process_rgba(dnr_context *ctx, const uint8_t *src, uint8_t *dst,
                             const dnr_controls *ctrl, float *out_network_ms);

/* 清空 temporal 历史（下行 seek / 帧序不连续时调用）。 */
DNR_API int dnr_reset_history(dnr_context *ctx);

/* 释放 context；会先等待 GPU 空闲。NULL 安全。 */
DNR_API void dnr_destroy(dnr_context *ctx);

/* 最近一次错误的详情（context 内部字符串，随下次调用失效）；NULL 安全。 */
DNR_API const char *dnr_last_error(const dnr_context *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DNR_C_H */
