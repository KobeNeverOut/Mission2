// 大恒（Daheng Imaging）工业相机取流主程序。
//
// 流程：初始化日志 -> 初始化 GalaxySDK -> 枚举 -> 按序列号打开 -> 配置 -> 取流 -> 显示 -> 收尾
//
// 调试信息约定：本项目所有诊断输出一律走 rm_log（内部是 spdlog 异步日志 + 滚动文件），
//               不出现任何 std::cout / printf。唯一例外是 --help 的用法文本，
//               它是给 shell 看的产品输出，不是日志。
//
// 用法见 --help。
#include "camera/daheng_camera.h"

#include "rm_log.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

namespace {

constexpr const char *kTag = "[main]";
constexpr const char *kDefaultWindow = "Daheng Galaxy Camera";

// 调参步长（键盘交互用）
constexpr double kExposureStepUs = 25.0;
constexpr double kGainStepDb = 0.1;
constexpr double kGammaStep = 0.1;

// 取流循环里连续失败多少次就认为相机掉线，收尾退出（正常超时不该无限重试）。
constexpr int kMaxConsecutiveFailures = 10;

std::atomic<bool> g_running{true};

void OnSignal(int /*signo*/) { g_running = false; }

// ---------------------------------------------------------------------------
// 帧率统计：按 1 秒窗口滑算，避免单帧抖动把读数带偏。
// ---------------------------------------------------------------------------
class FpsCounter {
public:
    void Tick() {
        ++frames_in_window_;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - window_start_).count();
        if (elapsed >= 1.0) {
            fps_ = static_cast<double>(frames_in_window_) / elapsed;
            frames_in_window_ = 0;
            window_start_ = now;
        }
    }

    double Fps() const { return fps_; }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point window_start_ = Clock::now();
    int    frames_in_window_ = 0;
    double fps_ = 0.0;
};

struct AppOptions {
    bool        show_help = false;
    bool        list_only = false;
    bool        display = true;     // --no-display / 无 X11 时自动关闭
    bool        use_index = false;  // 是否按 --index 打开
    uint32_t    device_index = 1;   // 大恒设备序号，从 1 开始
    std::string serial;             // --serial 指定的序列号
    int         frames = 0;         // 取多少帧后退出，0 = 一直取到 ESC/Ctrl+C
    int         save_every = 0;     // 每 N 帧存一张图，0 = 不自动存
    std::string save_dir = "snapshots";
    std::string log_file = "logs/daheng_camera.log";
    std::string console_level = "debug";
    std::string file_level = "info";
    std::string log_level = "debug";
    camera::CameraConfig config;
};

void PrintUsage(const char *program) {
    std::printf(
        "大恒工业相机取流程序（GalaxySDK + OpenCV + rm_log/spdlog）\n"
        "\n"
        "用法: %s [选项]\n"
        "\n"
        "设备:\n"
        "  -l, --list                只枚举设备并列出序列号/型号，不打开相机\n"
        "  -s, --serial <序列号>      按序列号打开相机（不填则用第一台）\n"
        "      --index <序号>         按大恒设备序号打开（序号从 1 开始，与 --list 输出一致）\n"
        "\n"
        "相机参数（可复现的另一半配置从代码里的 CameraConfig 默认值来）:\n"
        "  -e, --exposure <us>       曝光时间，微秒\n"
        "      --gain <dB>           增益，dB\n"
        "      --gamma <值>          伽马，<0 表示不动相机当前值\n"
        "      --pixel-format <名>   像素格式符号名，如 BayerRG8 / Mono8 / RGB8（默认 BayerRG8）\n"
        "      --width <px>          宽度，0 = 全幅\n"
        "      --height <px>         高度，0 = 全幅\n"
        "      --buffer-num <n>      SDK 取流缓冲个数（默认 5）\n"
        "      --timeout <ms>        单帧取流超时（默认 1000）\n"
        "      --no-max-throughput   不把 DeviceLinkThroughputLimit 拉满（默认拉满）\n"
        "\n"
        "运行:\n"
        "  -f, --frames <n>          取 n 帧后自动退出，0 = 手动退出（默认 0）\n"
        "      --no-display          不显示窗口（无 X11/Wayland 的机器、或做吞吐测试时用）\n"
        "      --save-dir <目录>     存图目录（默认 snapshots）\n"
        "      --save-every <n>      每 n 帧自动存一张 BGR 图，0 = 不自动存（默认 0）\n"
        "\n"
        "日志:\n"
        "      --log-file <路径>     日志文件（默认 logs/daheng_camera.log，滚动 5MB x 3）\n"
        "      --log-level <级别>    总级别: trace/debug/info/warn/error（默认 debug）\n"
        "      --console-level <级别> 控制台级别（默认 debug）\n"
        "      --file-level <级别>   文件级别（默认 info）\n"
        "\n"
        "  -h, --help                显示本帮助\n"
        "\n"
        "运行中按键（窗口有焦点时）:\n"
        "  e / d   曝光 +%g / -%g us\n"
        "  a / z   增益 +%g / -%g dB\n"
        "  s / w   伽马 +%g / -%g\n"
        "  p       立即存一张图\n"
        "  q / ESC 退出\n",
        program, kExposureStepUs, kExposureStepUs, kGainStepDb, kGainStepDb, kGammaStep, kGammaStep);
}

// 支持 "--key value" 与 "--key=value" 两种写法。
bool MatchOption(const std::string &arg, const char *long_name, const char *short_name,
                 std::string &inline_value, bool &has_inline_value) {
    const std::string long_prefix = std::string(long_name) + "=";
    if (arg == long_name || (short_name != nullptr && arg == short_name)) {
        has_inline_value = false;
        return true;
    }
    if (arg.rfind(long_prefix, 0) == 0) {
        inline_value = arg.substr(long_prefix.size());
        has_inline_value = true;
        return true;
    }
    return false;
}

bool NeedValue(int argc, char **argv, int &i, const std::string &inline_value, bool has_inline_value,
               const char *name, std::string &out) {
    if (has_inline_value) {
        out = inline_value;
        return true;
    }
    if (i + 1 >= argc) {
        std::fprintf(stderr, "选项 %s 缺少取值\n", name);
        return false;
    }
    out = argv[++i];
    return true;
}

bool ParseArgs(int argc, char **argv, AppOptions &opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        bool inline_value = false;

        if (arg == "-h" || arg == "--help") {
            opt.show_help = true;
        } else if (MatchOption(arg, "--list", "-l", value, inline_value)) {
            opt.list_only = true;
        } else if (MatchOption(arg, "--no-display", nullptr, value, inline_value)) {
            opt.display = false;
        } else if (MatchOption(arg, "--no-max-throughput", nullptr, value, inline_value)) {
            opt.config.maximize_throughput = false;
        } else if (MatchOption(arg, "--serial", "-s", value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--serial", opt.serial)) {
                return false;
            }
        } else if (MatchOption(arg, "--index", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--index", value)) {
                return false;
            }
            opt.device_index = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            opt.use_index = true;
        } else if (MatchOption(arg, "--exposure", "-e", value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--exposure", value)) {
                return false;
            }
            opt.config.exposure_time_us = std::strtod(value.c_str(), nullptr);
        } else if (MatchOption(arg, "--gain", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--gain", value)) {
                return false;
            }
            opt.config.gain_db = std::strtod(value.c_str(), nullptr);
        } else if (MatchOption(arg, "--gamma", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--gamma", value)) {
                return false;
            }
            opt.config.gamma = std::strtod(value.c_str(), nullptr);
        } else if (MatchOption(arg, "--pixel-format", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--pixel-format", value)) {
                return false;
            }
            opt.config.pixel_format = value;
        } else if (MatchOption(arg, "--width", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--width", value)) {
                return false;
            }
            opt.config.width = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--height", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--height", value)) {
                return false;
            }
            opt.config.height = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--buffer-num", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--buffer-num", value)) {
                return false;
            }
            opt.config.acquisition_buffer_num =
                static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--timeout", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--timeout", value)) {
                return false;
            }
            opt.config.frame_timeout_ms = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--frames", "-f", value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--frames", value)) {
                return false;
            }
            opt.frames = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--save-dir", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--save-dir", value)) {
                return false;
            }
            opt.save_dir = value;
        } else if (MatchOption(arg, "--save-every", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--save-every", value)) {
                return false;
            }
            opt.save_every = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (MatchOption(arg, "--log-file", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--log-file", value)) {
                return false;
            }
            opt.log_file = value;
        } else if (MatchOption(arg, "--log-level", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--log-level", value)) {
                return false;
            }
            opt.log_level = value;
        } else if (MatchOption(arg, "--console-level", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--console-level", value)) {
                return false;
            }
            opt.console_level = value;
        } else if (MatchOption(arg, "--file-level", nullptr, value, inline_value)) {
            if (!NeedValue(argc, argv, i, value, inline_value, "--file-level", value)) {
                return false;
            }
            opt.file_level = value;
        } else {
            std::fprintf(stderr, "无法识别的参数: %s（用 --help 查看用法）\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// 把命令行原样拼回去，写进日志便于复现现场。
std::string CommandLine(int argc, char **argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) {
            line += ' ';
        }
        line += argv[i];
    }
    return line;
}

std::string TimestampForFileName() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char text[32] = {0};
    std::strftime(text, sizeof(text), "%Y%m%d_%H%M%S", &tm_buf);
    return std::string(text);
}

// 存一张 BGR 图，返回是否成功。
bool SaveSnapshot(const cv::Mat &frame, const std::string &dir, uint64_t index) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        RM_LOG_ERROR("{} 创建存图目录 {} 失败: {}", kTag, dir, ec.message());
        return false;
    }
    const std::string path = dir + "/snapshot_" + TimestampForFileName() + "_" +
                             std::to_string(index) + ".png";
    if (!cv::imwrite(path, frame)) {
        RM_LOG_ERROR("{} 存图失败: {}", kTag, path);
        return false;
    }
    RM_LOG_INFO("{} 已存图: {} ({}x{})", kTag, path, frame.cols, frame.rows);
    return true;
}

// 键盘交互：返回 false 表示要求退出。
bool HandleKey(int key, camera::DahengCamera &camera, const AppOptions &opt, const cv::Mat &frame,
               uint64_t frame_index) {
    switch (key) {
        case 27:  // ESC
        case 'q':
            RM_LOG_INFO("{} 收到退出键，准备停止取流", kTag);
            return false;
        case 'e':
        case 'd': {
            double exposure = 0.0;
            if (!camera.ReadExposureTime(exposure)) {
                RM_LOG_WARN("{} 读取曝光失败，无法调整", kTag);
                break;
            }
            camera.SetExposureTime(exposure + (key == 'e' ? kExposureStepUs : -kExposureStepUs));
            break;
        }
        case 'a':
        case 'z': {
            double gain = 0.0;
            if (!camera.ReadGain(gain)) {
                RM_LOG_WARN("{} 读取增益失败，无法调整", kTag);
                break;
            }
            camera.SetGain(gain + (key == 'a' ? kGainStepDb : -kGainStepDb));
            break;
        }
        case 's':
        case 'w': {
            double gamma = 1.0;
            if (!camera.ReadGamma(gamma)) {
                RM_LOG_WARN("{} 读取伽马失败（该型号可能没有 Gamma 节点）", kTag);
                break;
            }
            camera.SetGamma(gamma + (key == 's' ? kGammaStep : -kGammaStep));
            break;
        }
        case 'p':
            SaveSnapshot(frame, opt.save_dir, frame_index);
            break;
        default:
            break;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    AppOptions options;
    if (!ParseArgs(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (options.show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    // ------------------------------------------------------------------
    // 1) 日志必须最先初始化：rm_log 的宏内部直接解引用 logger 指针，
    //    在 Init 之前调用任何一个 RM_LOG_* 都会段错误。
    // ------------------------------------------------------------------
    if (!options.log_file.empty()) {
        std::error_code ec;
        const std::filesystem::path log_path(options.log_file);
        if (log_path.has_parent_path()) {
            std::filesystem::create_directories(log_path.parent_path(), ec);
        }
    }
    try {
        INIT_LOG(options.log_file, options.console_level, options.file_level, options.log_level);
    } catch (const spdlog::spdlog_ex &e) {
        // 日志系统起不来就只剩 stderr 可用了，这是唯一的兜底输出。
        std::fprintf(stderr, "日志初始化失败: %s\n", e.what());
        return 1;
    }

    RM_LOG_INFO("================================================================");
    RM_LOG_INFO("{} 大恒工业相机取流程序启动", kTag);
    RM_LOG_INFO("{} 命令行: {}", kTag, CommandLine(argc, argv));
    RM_LOG_INFO("{} 日志文件: {} (console={} file={} level={})", kTag, options.log_file,
                options.console_level, options.file_level, options.log_level);

    // Ctrl+C / kill 也要走正常收尾流程，否则 SDK 句柄不会释放。
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 没有图形环境时自动降级为无窗口模式（WSL、纯命令行服务器常见）。
    if (options.display && std::getenv("DISPLAY") == nullptr &&
        std::getenv("WAYLAND_DISPLAY") == nullptr) {
        RM_LOG_WARN("{} 未检测到 DISPLAY / WAYLAND_DISPLAY，自动切换为 --no-display", kTag);
        options.display = false;
    }

    // ------------------------------------------------------------------
    // 2) GalaxySDK 全局初始化（大恒特有，海康没有这一步）
    // ------------------------------------------------------------------
    if (!camera::DahengCamera::InitLib()) {
        RM_LOG_CRITICAL("{} GalaxySDK 初始化失败，程序退出", kTag);
        RM_LOG_ERROR("{} 排查顺序: 1) 相机上电且插在 USB3.0 口(蓝色口) "
                     "2) Linux 需装好 GalaxySDK 的 udev 规则 /etc/udev/rules.d/99-galaxy-dev.rules "
                     "3) WSL 里必须先用 usbipd-win 把相机 attach 进发行版"
                     "(Windows 侧 usbipd list / usbipd attach)，否则 /dev/bus/usb 不存在、"
                     "SDK 会在 GXInitLib 就失败 4) 确认 libgxiapi.so 可被加载(ldd 无 not found)",
                     kTag);
        utils::RMLOG::instance().Close();
        return 1;
    }

    // ------------------------------------------------------------------
    // 3) 枚举设备
    // ------------------------------------------------------------------
    const std::vector<camera::DeviceInfo> devices = camera::DahengCamera::Enumerate(1000);
    if (devices.empty()) {
        RM_LOG_ERROR("{} 未找到可用相机，程序退出", kTag);
        camera::DahengCamera::CloseLib();
        utils::RMLOG::instance().Close();
        return 1;
    }
    for (const auto &device : devices) {
        RM_LOG_DEBUG("{} 设备 [{}] {} {} SN={} 别名={}", kTag, device.index, device.device_class,
                     device.model, device.serial, device.user_id);
    }

    if (options.list_only) {
        RM_LOG_INFO("{} --list 模式：共 {} 台设备，不打开相机", kTag, devices.size());
        camera::DahengCamera::CloseLib();
        utils::RMLOG::instance().Close();
        return 0;
    }

    // ------------------------------------------------------------------
    // 4) 打开相机：给了序列号按序列号（推荐，多相机时唯一可靠），否则用第一台
    // ------------------------------------------------------------------
    camera::DahengCamera camera;
    bool opened = false;
    if (options.use_index) {
        RM_LOG_INFO("{} 按设备序号打开: {}", kTag, options.device_index);
        opened = camera.OpenByIndex(options.device_index);
    } else if (!options.serial.empty()) {
        RM_LOG_INFO("{} 按序列号打开: {}", kTag, options.serial);
        opened = camera.OpenBySerial(options.serial);
    } else {
        const std::string serial = devices.front().serial;
        RM_LOG_WARN("{} 未指定 --serial，回退为第一台设备 SN={}（多相机场景请显式指定）", kTag,
                    serial);
        opened = camera.OpenBySerial(serial);
    }
    if (!opened) {
        RM_LOG_ERROR("{} 打开相机失败，程序退出", kTag);
        camera::DahengCamera::CloseLib();
        utils::RMLOG::instance().Close();
        return 1;
    }

    // ------------------------------------------------------------------
    // 5) 下发配置 + 开流
    // ------------------------------------------------------------------
    int exit_code = 0;
    if (!camera.Configure(options.config)) {
        RM_LOG_ERROR("{} 相机配置失败，程序退出", kTag);
        exit_code = 1;
    } else if (!camera.Start()) {
        RM_LOG_ERROR("{} 开流失败，程序退出", kTag);
        exit_code = 1;
    } else {
        RM_LOG_INFO("{} 取流开始，按 q/ESC 退出（窗口有焦点时）", kTag);
        if (options.display) {
            cv::namedWindow(kDefaultWindow, cv::WINDOW_AUTOSIZE);
        }

        FpsCounter fps_counter;
        uint64_t frame_index = 0;
        int consecutive_failures = 0;
        bool got_frame = false;
        const auto run_start = std::chrono::steady_clock::now();

        while (g_running) {
            cv::Mat frame;
            if (!camera.Grab(frame)) {
                // 超时/坏帧都会走到这里：连续多次失败说明相机掉线，及时收尾，
                // 而不是让日志以每帧一条的频率把磁盘刷满。
                if (++consecutive_failures >= kMaxConsecutiveFailures) {
                    RM_LOG_ERROR("{} 连续 {} 次取帧失败，判定相机掉线，准备退出", kTag,
                                 consecutive_failures);
                    exit_code = 1;
                    break;
                }
                continue;
            }
            consecutive_failures = 0;
            got_frame = true;
            ++frame_index;
            fps_counter.Tick();

            // 每帧都打 INFO 会把日志冲爆，这里只在 DEBUG 级记录单帧信息。
            RM_LOG_DEBUG("{} 帧 {} {}x{} fps={:.1f} frame_id={}", kTag, frame_index, frame.cols,
                         frame.rows, fps_counter.Fps(), camera.LastFrameId());

            if (options.save_every > 0 && (frame_index % static_cast<uint64_t>(options.save_every)) == 0) {
                SaveSnapshot(frame, options.save_dir, frame_index);
            }

            bool keep_running = true;
            if (options.display) {
                cv::Mat display = frame.clone();
                char overlay[64] = {0};
                std::snprintf(overlay, sizeof(overlay), "FPS: %.1f", fps_counter.Fps());
                cv::putText(display, overlay, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                            cv::Scalar(0, 255, 0), 2);
                try {
                    cv::imshow(kDefaultWindow, display);
                    const int key = cv::waitKey(1);
                    if (key != -1) {
                        keep_running = HandleKey(key, camera, options, frame, frame_index);
                    }
                } catch (const cv::Exception &e) {
                    // OpenCV 编译时没带 GUI 后端时会在这里抛异常，直接降级不中断取流。
                    RM_LOG_WARN("{} 显示失败({})，自动切换为 --no-display 继续取流", kTag, e.what());
                    options.display = false;
                }
            }

            if (!keep_running) {
                break;
            }
            if (options.frames > 0 && frame_index >= static_cast<uint64_t>(options.frames)) {
                RM_LOG_INFO("{} 已达到 --frames {} 帧上限，正常退出", kTag, options.frames);
                break;
            }
        }

        const double total_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
        const double average_fps =
            total_seconds > 0.0 ? static_cast<double>(frame_index) / total_seconds : 0.0;

        // ------------------------------------------------------------------
        // 6) 收尾：逆序关闭，先停流再关设备，最后关库（与 InitLib 配对）
        // ------------------------------------------------------------------
        camera.Stop();

        if (!got_frame) {
            RM_LOG_ERROR("{} 本次运行一帧都没取到：请检查曝光/触发/链路，或先用 --list 验证设备", kTag);
            exit_code = 1;
        }
        RM_LOG_INFO("{} 运行结束: 共 {} 帧, 平均 {:.2f} fps, 时长 {:.2f} s{}", kTag, frame_index,
                    average_fps, total_seconds, options.display ? "" : "（无窗口模式）");
        RM_LOG_INFO("{} SDK 侧统计: 收帧 {} / 坏帧 {} / 超时 {}", kTag, camera.ReceivedFrames(),
                    camera.DroppedFrames(), camera.TimeoutCount());
    }

    camera.Close();
    camera::DahengCamera::CloseLib();

    RM_LOG_INFO("{} 程序退出，退出码 {}", kTag, exit_code);
    utils::RMLOG::instance().Close();  // 异步日志必须先 shutdown 再退出，否则队列里的日志会丢
    return exit_code;
}
