// dnr_core.cpp - libdnr 的真实现：dnr_c.h 的 C ABI + dnr::Core（C++/Vulkan）。
//
// Vulkan 路径与 M1 spike（产出/spike/nr_host.cpp）逐行同源，已经验证过：
//   - RGBA8 UNORM 帧：runtime 走 Transfer::DirectBlit，答案按 k/255 回写，与 RGBA32F 路径逐字节相同；
//   - 持久 HOST_CACHED staging：HOST_VISIBLE|HOST_COHERENT 的 BAR 是 uncached 的，
//     从它 memcpy 出来约 200 MB/s（8 MB 就要 40 ms）；HOST_CACHED 后 2 ms 量级；
//   - 选卡不能靠 vulkaninfo 的序号（它被 VK_LAYER_MESA_device_select 重排过），要自己枚举。
#include "dnr_core.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>  // dup/dup2：把 runtime 的 printf 从 stdout 挪开（见 StdoutToStderr）
#include <vector>

#include "nr_log.hpp"   // nr::logf / set_log_sink：默认打到 stdout，库必须改道
#include "nrvk.hpp"
#include "nr_runtime.hpp"

namespace dnr {
namespace {

constexpr const char* kDefaultRoot = "/home/retro/dlssnr-test";
constexpr const char* kVersion = "dnr 0.1.0";

bool has_device_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
    for (const auto& x : e)
        if (!std::strcmp(x.extensionName, name)) return true;
    return false;
}

// transition / copy / transition 记进一个 command buffer，提交一次。
// 图像进出一律回到 GENERAL（ColourFrame 的默认前后布局）。
void image_copy(nrvk::Context& ctx, nrvk::Context::Image& im, VkBuffer stage, bool to_gpu) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.handle;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageLayout from = im.layout;
    const VkImageLayout mid =
        to_gpu ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ctx.one_shot([&](VkCommandBuffer cmd) {
        b.oldLayout = from;
        b.newLayout = mid;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {im.w, im.h, 1};
        if (to_gpu)
            vkCmdCopyBufferToImage(cmd, stage, im.handle, mid, 1, &r);
        else
            vkCmdCopyImageToBuffer(cmd, im.handle, mid, stage, 1, &r);
        b.oldLayout = mid;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | (to_gpu ? 0 : VK_ACCESS_TRANSFER_READ_BIT);
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    });
    im.layout = VK_IMAGE_LAYOUT_GENERAL;
}

// 持久 staging：优先 HOST_VISIBLE|HOST_CACHED（可选再加 COHERENT）。
struct Staging {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkDeviceSize bytes{};
    bool coherent{};
};

Staging make_staging(nrvk::Context& ctx, VkDeviceSize bytes) {
    Staging s;
    s.bytes = bytes;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    NRVK_CHECK(vkCreateBuffer(ctx.device, &bi, nullptr, &s.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx.device, s.buffer, &req);

    auto find = [&](VkMemoryPropertyFlags want) -> uint32_t {
        for (uint32_t i = 0; i < ctx.mem.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = ctx.mem.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (f & want) == want) return i;
        }
        return ctx.mem.memoryTypeCount;
    };
    // cached 优先（回读快）；没有再退回 plain host-visible coherent。
    uint32_t type = find(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == ctx.mem.memoryTypeCount)
        type = find(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == ctx.mem.memoryTypeCount) throw std::runtime_error("no host-visible memory type for staging");

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    NRVK_CHECK(vkAllocateMemory(ctx.device, &ai, nullptr, &s.memory));
    NRVK_CHECK(vkBindBufferMemory(ctx.device, s.buffer, s.memory, 0));
    NRVK_CHECK(vkMapMemory(ctx.device, s.memory, 0, bytes, 0, &s.mapped));
    s.coherent = (ctx.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return s;
}

void destroy_staging(nrvk::Context& ctx, Staging& s) {
    if (s.mapped) vkUnmapMemory(ctx.device, s.memory);
    if (s.buffer) vkDestroyBuffer(ctx.device, s.buffer, nullptr);
    if (s.memory) vkFreeMemory(ctx.device, s.memory, nullptr);
    s = {};
}

void staging_invalidate(nrvk::Context& ctx, Staging& s) {
    if (s.coherent) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = s.memory;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    NRVK_CHECK(vkInvalidateMappedMemoryRanges(ctx.device, 1, &r));
}

void staging_flush(nrvk::Context& ctx, Staging& s) {
    if (s.coherent) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = s.memory;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    NRVK_CHECK(vkFlushMappedMemoryRanges(ctx.device, 1, &r));
}

// runtime（nr::logf）默认把诊断打到 **stdout**。对 dnr-video 这种 stdout 就是像素流的
// 程序这是致命的：编码端会在最后一帧后面多收到几千字节日志，报
// "Invalid buffer size, packet size 3558 < expected frame_size 8294400"。
// 库不能写 stdout，所以 create 时把 sink 换到 stderr（verbose=0 时丢弃）。
void runtime_log_stderr(const char* line) { std::fprintf(stderr, "%s\n", line); }
void runtime_log_silent(const char*) {}

// nr_graph.cpp 里还有一批 process 级别的 std::printf（"model pack:", "pre conditioning [..]",
// "Overall Intensity: ..." 之类），不走 nr::logf，sink 拦不到。它们在构建期各打一次，
// 控制强度变化时也可能再打。对 dnr-video 来说 stdout 是像素流，任何一行都会让编码端收到
// "packet size 2586 < expected frame_size"。所以构造 Runtime（以及每帧 record）期间，
// 把 fd 1 临时接到 fd 2，结束后原样接回；不改上游源码。
class StdoutToStderr {
public:
    StdoutToStderr() {
        std::fflush(stdout);
        saved_ = ::dup(STDOUT_FILENO);
        if (saved_ >= 0) ::dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    ~StdoutToStderr() {
        std::fflush(stdout);
        if (saved_ >= 0) {
            ::dup2(saved_, STDOUT_FILENO);
            ::close(saved_);
        }
    }
    StdoutToStderr(const StdoutToStderr&) = delete;
    StdoutToStderr& operator=(const StdoutToStderr&) = delete;

private:
    int saved_ = -1;
};

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}

}  // namespace

std::string resolve_root(const dnr_config& cfg) {
    if (cfg.root && *cfg.root) return std::string(cfg.root);
    if (const char* e = std::getenv("DNR_ROOT"); e && *e) return std::string(e);
    return std::string(kDefaultRoot);
}

std::string select_gpu(const dnr_config& cfg) {
    if (cfg.gpu_index >= 0) {
        char b[16];
        std::snprintf(b, sizeof b, "%d", cfg.gpu_index);
        setenv("NR_GPU", b, 1);
        return fmt("device index %d (caller)", cfg.gpu_index);
    }
    if (const char* e = std::getenv("NR_GPU"); e && *e) return fmt("NR_GPU=%s (environment)", e);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst{};
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS)
        throw std::runtime_error("vkCreateInstance failed while probing devices");
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    int chosen = -1;
    std::string name;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        const bool fp8 = has_device_extension(devs[i], VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        const bool coop = has_device_extension(devs[i], VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        if (cfg.verbose >= 2)
            std::fprintf(stderr, "[dnr] gpu %u: %04x:%04x %s | coop_matrix %s | float8 %s\n", i,
                         p.vendorID, p.deviceID, p.deviceName, coop ? "yes" : "no", fp8 ? "yes" : "no");
        if (chosen < 0 && p.vendorID == 0x1002 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            coop && fp8) {
            chosen = int(i);
            name = p.deviceName;
        }
    }
    vkDestroyInstance(inst, nullptr);
    if (chosen < 0)
        throw std::runtime_error(
            "no AMD discrete GPU with VK_KHR_cooperative_matrix + VK_EXT_shader_float8; "
            "set dnr_config.gpu_index or NR_GPU");
    char b[16];
    std::snprintf(b, sizeof b, "%d", chosen);
    setenv("NR_GPU", b, 1);
    return fmt("%s (index %d, auto)", name.c_str(), chosen);
}

struct Core::Impl {
    nrvk::Context ctx;
    nr::HostDevice host{};
    std::unique_ptr<nr::Runtime> rt;
    nrvk::Context::Image img{};
    Staging stage{};
    uint32_t width{}, height{};
    bool temporal{};
    bool pipeline_cache_hit{};
    std::string device_name, driver_name;
    std::mutex mu;
    int verbose{1};
};

Core::Core(const dnr_config& cfg, std::string& error) : impl_(new Impl) {
    Impl& s = *impl_;
    s.width = cfg.width;
    s.height = cfg.height;
    s.temporal = cfg.temporal != 0;
    s.verbose = cfg.verbose;
    // 把 runtime 的诊断从 stdout 挪走（verbose=0 时直接丢弃）。必须在 Runtime 构造之前设。
    nr::set_log_sink(cfg.verbose > 0 ? runtime_log_stderr : runtime_log_silent);
    const std::string root = resolve_root(cfg);

    try {
        const std::string gpu = select_gpu(cfg);
        if (cfg.verbose >= 1) std::fprintf(stderr, "[dnr] gpu: %s\n", gpu.c_str());
        s.ctx.create();
        s.device_name = s.ctx.gpu_name;
        s.driver_name = s.ctx.driver_name;
        if (cfg.verbose >= 1)
            std::fprintf(stderr, "[dnr] device: %s | %s | queue family %u\n", s.ctx.gpu_name.c_str(),
                         s.ctx.driver_name.c_str(), s.ctx.family);

        s.host.instance = s.ctx.instance;
        s.host.physical = s.ctx.physical;
        s.host.device = s.ctx.device;
        s.host.queue = s.ctx.queue;
        s.host.queue_family = s.ctx.family;

        nr::RuntimeConfig rc{};
        rc.root = root;
        rc.width = cfg.width;
        rc.height = cfg.height;
        rc.colour_format = VK_FORMAT_R8G8B8A8_UNORM;
        rc.linear_input = false;  // SDR, sRGB 编码 —— 模型训练域（01_设计 §7）
        rc.model_scale = cfg.model_scale > 0.0f ? cfg.model_scale : 1.0f;
        rc.max_passes = 1;

        const double t0 = [&] {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        }();
        {
            StdoutToStderr quiet_stdout;  // 构建期的 std::printf 全部落到 stderr
            if (s.temporal) {
                nr::TemporalConfig tc{};
                tc.enable = true;
                tc.history_strength = 1.0f;
                s.rt.reset(new nr::Runtime(s.host, rc, nr::ControlMaskConfig{}, tc));
            } else {
                s.rt.reset(new nr::Runtime(s.host, rc));
            }
        }
        const double t1 = [&] {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        }();
        s.pipeline_cache_hit = (t1 - t0) < 5000.0;  // 冷启动 20 s 量级，缓存命中 1 s 内
        if (cfg.verbose >= 1)
            std::fprintf(stderr, "[dnr] runtime %ux%u%s built in %.0f ms (model extent %ux%u)\n",
                         cfg.width, cfg.height, s.temporal ? " temporal" : "", t1 - t0,
                         s.rt->model_width(), s.rt->model_height());

        s.img = s.ctx.image(cfg.width, cfg.height, VK_FORMAT_R8G8B8A8_UNORM, false, true, true);
        s.stage = make_staging(s.ctx, VkDeviceSize(cfg.width) * cfg.height * 4);
    } catch (const std::exception& e) {
        error = e.what();
        throw;
    }
}

Core::~Core() {
    if (!impl_) return;
    Impl& s = *impl_;
    try {
        vkDeviceWaitIdle(s.ctx.device);
        if (s.stage.buffer) destroy_staging(s.ctx, s.stage);
        if (s.img.handle) s.ctx.destroy(s.img);
        s.rt.reset();
        s.ctx.destroy();
    } catch (...) {
        // 析构不能抛
    }
    nr::set_log_sink(nullptr);
}

int Core::process(const uint8_t* src, uint8_t* dst, const dnr_controls& ctrl, float* out_network_ms,
                  std::string& error) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mu);
    try {
        const VkDeviceSize bytes = VkDeviceSize(s.width) * s.height * 4;

        // 1) CPU -> staging -> image。先拷进 staging，src == dst 的 in-place 调用才安全。
        std::memcpy(s.stage.mapped, src, size_t(bytes));
        staging_flush(s.ctx, s.stage);
        image_copy(s.ctx, s.img, s.stage.buffer, true);

        // 2) 一帧 NR。record() 只记录，submit + fence 由这里做。
        nr::ColourFrame frame{};
        frame.image = s.img.handle;
        frame.format = VK_FORMAT_R8G8B8A8_UNORM;
        frame.width = s.width;
        frame.height = s.height;
        frame.before = VK_IMAGE_LAYOUT_GENERAL;
        frame.after = VK_IMAGE_LAYOUT_GENERAL;
        frame.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        nr::Controls c{};
        c.enabled = true;
        c.intensity = ctrl.intensity > 0.0f ? ctrl.intensity : 1.0f;
        c.style = ctrl.style;

        const bool reset = ctrl.reset_history != 0;
        {
            StdoutToStderr quiet_stdout;  // 强度/控制变化时 record 里也可能 printf
            if (s.temporal) {
                nr::TemporalFrame tf{};
                tf.reset = reset;
                s.ctx.one_shot([&](VkCommandBuffer cmd) { s.rt->record_temporal(cmd, frame, c, tf); });
            } else {
                s.ctx.one_shot([&](VkCommandBuffer cmd) { s.rt->record(cmd, frame, c, reset); });
            }
        }

        // 3) image -> staging -> CPU
        image_copy(s.ctx, s.img, s.stage.buffer, false);
        staging_invalidate(s.ctx, s.stage);
        std::memcpy(dst, s.stage.mapped, size_t(bytes));

        if (out_network_ms) *out_network_ms = s.rt->average_network_ms();
        return DNR_OK;
    } catch (const std::bad_alloc& e) {
        error = fmt("out of memory: %s", e.what());
        return DNR_ERR_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        error = e.what();
        return DNR_ERR_PROCESS;
    }
}

int Core::reset_history(std::string& error) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mu);
    try {
        s.rt->forget_history();
        return DNR_OK;
    } catch (const std::exception& e) {
        error = e.what();
        return DNR_ERR_INTERNAL;
    }
}

float Core::last_gpu_ms() const { return impl_->rt ? impl_->rt->last_gpu_ms() : 0.0f; }
float Core::average_network_ms() const {
    return impl_->rt ? impl_->rt->average_network_ms() : 0.0f;
}
uint32_t Core::model_width() const { return impl_->rt ? impl_->rt->model_width() : 0; }
uint32_t Core::model_height() const { return impl_->rt ? impl_->rt->model_height() : 0; }
bool Core::temporal() const { return impl_->temporal; }
const std::string& Core::device_name() const { return impl_->device_name; }
const std::string& Core::driver_name() const { return impl_->driver_name; }
bool Core::pipeline_cache_hit() const { return impl_->pipeline_cache_hit; }

}  // namespace dnr

// ---------------------------------------------------------------------------
// C ABI（dnr_c.h）。dnr_context 的实现细节只在这里出现。
// ---------------------------------------------------------------------------

struct dnr_context {
    dnr_config cfg{};      // 调用方结构体的副本（root 指向 owned_root）
    std::string owned_root;
    std::unique_ptr<dnr::Core> core;
    std::string last_error;
};

namespace {

void set_err(dnr_context* ctx, const char* msg, char* errbuf, size_t errbuf_size) {
    const char* text = msg ? msg : "";
    if (ctx) ctx->last_error = text;
    if (errbuf && errbuf_size) {
        std::snprintf(errbuf, errbuf_size, "%s", text);
        errbuf[errbuf_size - 1] = '\0';
    }
}

// 把构造期抛出的异常文本分类成 dnr_status：设备/扩展问题 vs 权重与 shader 路径问题。
// 分类只影响调用方看到的码，错误文本始终是原始 what()，不丢信息。
int status_for(const std::string& msg) {
    static const char* kDevice[] = {"no AMD discrete GPU", "vkCreateInstance", "device feature missing",
                                    "subgroup", "cooperative", "subgroup config", "32-lane"};
    static const char* kModel[] = {"dlssnr-amd", "shader", "model", "NR graph", "pack", "dlssnr.bin",
                                   "canonical", "installed tree"};
    for (const char* k : kDevice)
        if (msg.find(k) != std::string::npos) return DNR_ERR_DEVICE;
    for (const char* k : kModel)
        if (msg.find(k) != std::string::npos) return DNR_ERR_MODEL;
    if (msg.find("memory") != std::string::npos) return DNR_ERR_OUT_OF_MEMORY;
    return DNR_ERR_INTERNAL;
}

}  // namespace

extern "C" {

const char* dnr_version(void) { return dnr::kVersion; }

const char* dnr_status_string(int status) {
    switch (status) {
        case DNR_OK: return "ok";
        case DNR_ERR_INVALID_ARG: return "invalid argument";
        case DNR_ERR_DEVICE: return "device error (no usable GPU / missing Vulkan feature)";
        case DNR_ERR_MODEL: return "model error (weights, shaders or root path unusable)";
        case DNR_ERR_PROCESS: return "process error (record or readback failed)";
        case DNR_ERR_UNSUPPORTED: return "unsupported (format or feature)";
        case DNR_ERR_OUT_OF_MEMORY: return "out of memory";
        case DNR_ERR_INTERNAL: return "internal error";
        default: return "unknown error";
    }
}

dnr_context* dnr_create(const dnr_config* cfg, char* errbuf, size_t errbuf_size) {
    if (!cfg) {
        set_err(nullptr, "dnr_create: null config", errbuf, errbuf_size);
        return nullptr;
    }
    if (cfg->width == 0 || cfg->height == 0) {
        set_err(nullptr, "dnr_create: width/height must be > 0", errbuf, errbuf_size);
        return nullptr;
    }
    if (cfg->cfg_size != 0 && cfg->cfg_size < sizeof(dnr_config)) {
        set_err(nullptr, "dnr_create: cfg_size smaller than this build's dnr_config", errbuf, errbuf_size);
        return nullptr;
    }
    if (cfg->model_scale < 0.0f || cfg->model_scale > 1.0f) {
        set_err(nullptr, "dnr_create: model_scale must be in (0, 1]", errbuf, errbuf_size);
        return nullptr;
    }

    std::unique_ptr<dnr_context> ctx(new (std::nothrow) dnr_context());
    if (!ctx) {
        set_err(nullptr, "dnr_create: out of memory", errbuf, errbuf_size);
        return nullptr;
    }
    ctx->cfg = *cfg;
    ctx->owned_root = dnr::resolve_root(*cfg);   // 调用方的 root 指针不持有
    ctx->cfg.root = ctx->owned_root.c_str();
    if (ctx->cfg.cfg_size == 0) ctx->cfg.cfg_size = sizeof(dnr_config);

    std::string err;
    try {
        ctx->core.reset(new dnr::Core(ctx->cfg, err));
    } catch (const std::exception& e) {
        err = e.what();
        set_err(ctx.get(), err.c_str(), errbuf, errbuf_size);
        const int st = status_for(err);
        if (errbuf && errbuf_size)
            std::snprintf(errbuf, errbuf_size, "[%s] %s", dnr_status_string(st), err.c_str());
        return nullptr;
    }
    return ctx.release();
}

int dnr_process_rgba(dnr_context* ctx, const uint8_t* src, uint8_t* dst, const dnr_controls* ctrl,
                     float* out_network_ms) {
    if (!ctx || !ctx->core) return DNR_ERR_INVALID_ARG;
    if (!src || !dst) {
        ctx->last_error = "dnr_process_rgba: null src or dst";
        return DNR_ERR_INVALID_ARG;
    }
    if (ctrl && ctrl->ctrl_size != 0 && ctrl->ctrl_size < sizeof(dnr_controls)) {
        ctx->last_error = "dnr_process_rgba: ctrl_size smaller than this build's dnr_controls";
        return DNR_ERR_INVALID_ARG;
    }
    dnr_controls c{};
    c.intensity = 1.0f;
    c.style = 0;
    c.reset_history = 0;
    if (ctrl) {
        c.intensity = ctrl->intensity;
        c.style = ctrl->style;
        c.reset_history = ctrl->reset_history;
    }
    if (out_network_ms) *out_network_ms = 0.0f;

    std::string err;
    const int st = ctx->core->process(src, dst, c, out_network_ms, err);
    if (st != DNR_OK) ctx->last_error = err;
    return st;
}

int dnr_reset_history(dnr_context* ctx) {
    if (!ctx || !ctx->core) return DNR_ERR_INVALID_ARG;
    std::string err;
    const int st = ctx->core->reset_history(err);
    if (st != DNR_OK) ctx->last_error = err;
    return st;
}

void dnr_destroy(dnr_context* ctx) { delete ctx; }

const char* dnr_last_error(const dnr_context* ctx) {
    if (!ctx) return "no context";
    return ctx->last_error.empty() ? "" : ctx->last_error.c_str();
}

}  // extern "C"
