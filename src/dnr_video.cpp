// dnr_video.cpp - dnr-video：裸 RGBA8 进、裸 RGBA8 出的 CLI（M2）。
//
// 只用 stdin/stdout 或普通文件，不链 FFmpeg（设计 01 §3）：解码/编码交给 ffmpeg，
// 这个程序只做 "一帧 RGBA8 进 → libdnr → 一帧 RGBA8 出"。
//
//   解码 | dnr-video | 编码
//   ffmpeg -i in.mp4 -vf "scale=1920:1080,format=rgba" -f rawvideo -pix_fmt rgba - \
//     | dnr-video --width 1920 --height 1080 --root /home/retro/dlssnr-test \
//     | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - -c:v libx264 -crf 18 out.mp4
//
// 输出流上不能有别的字节：所有日志都走 stderr。
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dnr_c.h"

namespace {

struct Args {
    std::string in = "-", out = "-", root;
    uint32_t width = 0, height = 0;
    long frames = 0;        // 0 = 读到 EOF
    bool bench = false;     // 不写输出，只测
    float intensity = 1.0f;
    float model_scale = 1.0f;
    int style = 0;
    int gpu = -1;
    int verbose = 1;
    bool temporal = true;   // 视频默认开 temporal（01_设计 §2.1）
    long reset_every = 0;   // 每 N 帧清一次历史（0 = 不清）
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --width W --height H [--in FILE|-] [--out FILE|-] [--root DIR]\n"
                 "          [--frames N] [--bench] [--intensity F] [--style N]\n"
                 "          [--temporal|--no-temporal] [--reset-every N] [--model-scale F]\n"
                 "          [--gpu N] [--quiet]\n"
                 "\n"
                 "  裸 RGBA8（每帧 width*height*4 字节，行优先紧排）进 / 出。\n"
                 "  --bench 只测速不写输出。日志全部走 stderr，stdout 只有像素。\n",
                 argv0);
}

bool parse(int argc, char** argv, Args& a) {
    auto need = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "dnr-video: %s 需要参数\n", name);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--in") {
            const char* v = need(i, "--in");
            if (!v) return false;
            a.in = v;
        } else if (k == "--out") {
            const char* v = need(i, "--out");
            if (!v) return false;
            a.out = v;
        } else if (k == "--root") {
            const char* v = need(i, "--root");
            if (!v) return false;
            a.root = v;
        } else if (k == "--width") {
            const char* v = need(i, "--width");
            if (!v) return false;
            a.width = uint32_t(std::strtoul(v, nullptr, 10));
        } else if (k == "--height") {
            const char* v = need(i, "--height");
            if (!v) return false;
            a.height = uint32_t(std::strtoul(v, nullptr, 10));
        } else if (k == "--frames") {
            const char* v = need(i, "--frames");
            if (!v) return false;
            a.frames = std::strtol(v, nullptr, 10);
        } else if (k == "--intensity") {
            const char* v = need(i, "--intensity");
            if (!v) return false;
            a.intensity = float(std::strtod(v, nullptr));
        } else if (k == "--model-scale") {
            const char* v = need(i, "--model-scale");
            if (!v) return false;
            a.model_scale = float(std::strtod(v, nullptr));
        } else if (k == "--style") {
            const char* v = need(i, "--style");
            if (!v) return false;
            a.style = int(std::strtol(v, nullptr, 10));
        } else if (k == "--gpu") {
            const char* v = need(i, "--gpu");
            if (!v) return false;
            a.gpu = int(std::strtol(v, nullptr, 10));
        } else if (k == "--reset-every") {
            const char* v = need(i, "--reset-every");
            if (!v) return false;
            a.reset_every = std::strtol(v, nullptr, 10);
        } else if (k == "--bench") {
            a.bench = true;
        } else if (k == "--temporal") {
            a.temporal = true;
        } else if (k == "--no-temporal") {
            a.temporal = false;
        } else if (k == "--quiet") {
            a.verbose = 0;
        } else if (k == "--help" || k == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "dnr-video: 未知参数 %s\n", k.c_str());
            return false;
        }
    }
    if (a.width == 0 || a.height == 0) {
        std::fprintf(stderr, "dnr-video: --width/--height 必填\n");
        return false;
    }
    return true;
}

std::FILE* open_in(const std::string& path) {
    if (path == "-") return stdin;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) std::fprintf(stderr, "dnr-video: 打不开输入 %s: %s\n", path.c_str(), std::strerror(errno));
    return f;
}

std::FILE* open_out(const std::string& path) {
    if (path == "-") return stdout;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) std::fprintf(stderr, "dnr-video: 打不开输出 %s: %s\n", path.c_str(), std::strerror(errno));
    return f;
}

double mono_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) {
        usage(argv[0]);
        return 2;
    }
    const size_t frame_bytes = size_t(a.width) * a.height * 4;

    dnr_config cfg{};
    cfg.root = a.root.empty() ? nullptr : a.root.c_str();
    cfg.width = a.width;
    cfg.height = a.height;
    cfg.temporal = a.temporal ? 1 : 0;
    cfg.model_scale = a.model_scale;
    cfg.gpu_index = a.gpu;
    cfg.verbose = a.verbose;
    cfg.cfg_size = sizeof(dnr_config);

    char errbuf[1024] = {0};
    std::fprintf(stderr, "[dnr-video] %s | %ux%u | temporal=%s | intensity=%.2f%s\n", dnr_version(),
                 a.width, a.height, a.temporal ? "on" : "off", a.intensity,
                 a.bench ? " | bench" : "");
    const double t_create0 = mono_ms();
    dnr_context* ctx = dnr_create(&cfg, errbuf, sizeof errbuf);
    const double t_create1 = mono_ms();
    if (!ctx) {
        std::fprintf(stderr, "[dnr-video] dnr_create 失败: %s\n", errbuf[0] ? errbuf : "(no message)");
        return 3;
    }
    std::fprintf(stderr, "[dnr-video] context 就绪，用时 %.0f ms\n", t_create1 - t_create0);

    std::FILE* fi = open_in(a.in);
    if (!fi) {
        dnr_destroy(ctx);
        return 3;
    }
    std::FILE* fo = nullptr;
    if (!a.bench) {
        fo = open_out(a.out);
        if (!fo) {
            if (fi != stdin) std::fclose(fi);
            dnr_destroy(ctx);
            return 3;
        }
    }

    std::vector<unsigned char> src(frame_bytes), dst(frame_bytes);
    double sum_net = 0.0, sum_total = 0.0;
    double t_start = mono_ms();
    long done = 0;
    int rc = 0;

    for (long frame = 0; a.frames == 0 || frame < a.frames; ++frame) {
        const size_t got = std::fread(src.data(), 1, frame_bytes, fi);
        if (got == 0) break;
        if (got != frame_bytes) {
            std::fprintf(stderr, "[dnr-video] 第 %ld 帧不完整（%zu/%zu 字节），当作流结束\n", frame, got,
                         frame_bytes);
            break;
        }

        dnr_controls ctrl{};
        ctrl.intensity = a.intensity;
        ctrl.style = a.style;
        ctrl.reset_history = (a.reset_every > 0 && frame > 0 && frame % a.reset_every == 0) ? 1 : 0;
        ctrl.ctrl_size = sizeof(dnr_controls);

        float net_ms = 0.0f;
        const double t0 = mono_ms();
        const int st = dnr_process_rgba(ctx, src.data(), dst.data(), &ctrl, &net_ms);
        const double t1 = mono_ms();
        if (st != DNR_OK) {
            std::fprintf(stderr, "[dnr-video] 第 %ld 帧失败: %s (%s)\n", frame, dnr_status_string(st),
                         dnr_last_error(ctx));
            rc = 4;
            break;
        }

        if (fo && std::fwrite(dst.data(), 1, frame_bytes, fo) != frame_bytes) {
            std::fprintf(stderr, "[dnr-video] 写输出失败: %s\n", std::strerror(errno));
            rc = 5;
            break;
        }

        const double total_ms = t1 - t0;
        sum_net += net_ms;
        sum_total += total_ms;
        ++done;
        if (a.verbose >= 1 && (frame % 30 == 29))
            std::fprintf(stderr, "[dnr-video] frame %ld | net %.2f ms | total %.2f ms | %.1f fps\n",
                         frame, net_ms, total_ms, total_ms > 0 ? 1000.0 / total_ms : 0.0);
    }

    const double wall_ms = mono_ms() - t_start;
    if (fo && fo != stdout) std::fclose(fo);
    if (fo == stdout) std::fflush(stdout);
    if (fi != stdin) std::fclose(fi);

    if (done > 0) {
        const double avg_net = sum_net / done, avg_total = sum_total / done;
        std::fprintf(stderr,
                     "[dnr-video] %ld 帧 | avg net %.2f ms | avg total %.2f ms | %.1f fps"
                     " | wall %.0f ms (%.1f fps 含 I/O)\n",
                     done, avg_net, avg_total, avg_total > 0 ? 1000.0 / avg_total : 0.0, wall_ms,
                     wall_ms > 0 ? done * 1000.0 / wall_ms : 0.0);
    } else {
        std::fprintf(stderr, "[dnr-video] 没有处理任何帧\n");
        if (rc == 0) rc = 6;
    }

    dnr_destroy(ctx);
    return rc;
}
