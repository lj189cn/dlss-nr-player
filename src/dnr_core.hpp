// dnr_core.hpp - libdnr 的内部 C++ 接口（不对外；对外只有 dnr_c.h）。
//
// dnr::Core 封装 DLSSNR-AMD 的 nr::Runtime：
//   RGBA8 帧 -> VkImage(R8G8B8A8_UNORM, GENERAL) -> record()/record_temporal()
//            -> 持久 HOST_CACHED staging 回读 -> RGBA8
//
// 参数与 M1 spike 完全一致（见 产出/03_测试记录.md）：linear_input=false、
// colour_format=R8G8B8A8_UNORM（走 runtime 的 Transfer::DirectBlit + k/255 回写）、
// 单帧 record / temporal record_temporal（history_strength=1.0，第 0 帧 reset）。
//
// 线程：Core 内部有一把 mutex，process/reset_history 串行化；调用方仍然不应该
// 在多线程里并发用同一个 context（dnr_c.h 的约定）。
#pragma once

#include <memory>
#include <string>

#include "dnr_c.h"

namespace dnr {

class Core {
public:
    // 构造会创建 Vulkan 设备、上传权重、编译 pipeline（首次 20 s 量级）。
    // 失败抛 std::runtime_error；错误码由 dnr_create 按消息分类。
    Core(const dnr_config& cfg, std::string& error);
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // 一帧 RGBA8（sRGB 编码 SDR）。src 与 dst 可以是同一块内存。
    // out_network_ms 可为空；非空时写回 runtime 自报的网络耗时（ms，最近几帧平滑）。
    int process(const uint8_t* src, uint8_t* dst, const dnr_controls& ctrl, float* out_network_ms,
                std::string& error);

    // 丢弃 temporal 历史（下一次 record_temporal 当新序列处理）。
    int reset_history(std::string& error);

    // 给 CLI 用的统计量（最近一次 process 结束后的读数）。
    float last_gpu_ms() const;
    float average_network_ms() const;
    uint32_t model_width() const;
    uint32_t model_height() const;
    bool temporal() const;
    const std::string& device_name() const;
    const std::string& driver_name() const;
    bool pipeline_cache_hit() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 把 cfg 里的 root 解析成实际路径（cfg->root / DNR_ROOT / 内置默认）。
std::string resolve_root(const dnr_config& cfg);

// 选卡：gpu_index >= 0 直接用它；< 0 且环境里没有 NR_GPU 时，枚举并选中
// 「第一个同时有 VK_KHR_cooperative_matrix + VK_EXT_shader_float8 的 AMD 独显」。
// 必须在 nrvk::Context::create() 之前调用（create() 只认 NR_GPU）。
// 返回选中的设备名（可能为空），选卡失败时抛 std::runtime_error。
std::string select_gpu(const dnr_config& cfg);

}  // namespace dnr
