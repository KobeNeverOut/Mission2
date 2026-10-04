#include "camera/daheng_camera.h"

#include "rm_log.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>

namespace camera {
namespace {

// 日志统一前缀：rm_log 的 pattern 里已经有 [文件 函数:行号]，
// 这里再加一个模块标签，多模块工程里 grep "camera" / "main" 就能分流。
constexpr const char *kTag = "[camera]";

// SDK 里的字符串字段是 unsigned char[N]，且不保证以 '\0' 结尾，必须自己截断。
template <size_t N>
std::string FixedString(const unsigned char (&field)[N]) {
    size_t len = 0;
    while (len < N && field[len] != '\0') {
        ++len;
    }
    return std::string(reinterpret_cast<const char *>(field), len);
}

// GX_ENUM_VALUE_DES::strCurSymbolic 同理。
std::string Symbolic(const GX_ENUM_VALUE_DES &des) {
    size_t len = 0;
    while (len < sizeof(des.strCurSymbolic) && des.strCurSymbolic[len] != '\0') {
        ++len;
    }
    return std::string(des.strCurSymbolic, len);
}

const char *DeviceClassName(GX_DEVICE_CLASS device_class) {
    switch (device_class) {
        case GX_DEVICE_CLASS_U3V: return "U3V(USB3.0)";
        case GX_DEVICE_CLASS_USB2: return "USB2.0";
        case GX_DEVICE_CLASS_GEV: return "GEV(GigE)";
        case GX_DEVICE_CLASS_CXP: return "CXP";
        case GX_DEVICE_CLASS_SMART: return "SMART";
        default: return "UNKNOWN";
    }
}

const char *PixelFormatName(int64_t format) {
    switch (format) {
        case GX_PIXEL_FORMAT_MONO8: return "Mono8";
        case GX_PIXEL_FORMAT_MONO10: return "Mono10";
        case GX_PIXEL_FORMAT_MONO12: return "Mono12";
        case GX_PIXEL_FORMAT_BAYER_RG8: return "BayerRG8";
        case GX_PIXEL_FORMAT_BAYER_GB8: return "BayerGB8";
        case GX_PIXEL_FORMAT_BAYER_GR8: return "BayerGR8";
        case GX_PIXEL_FORMAT_BAYER_BG8: return "BayerBG8";
        case GX_PIXEL_FORMAT_RGB8: return "RGB8";
        case GX_PIXEL_FORMAT_BGR8: return "BGR8";
        default: return "Other";
    }
}

const char *ColorFilterName(int64_t filter) {
    switch (filter) {
        case GX_COLOR_FILTER_BAYER_RG: return "RG";
        case GX_COLOR_FILTER_BAYER_GB: return "GB";
        case GX_COLOR_FILTER_BAYER_GR: return "GR";
        case GX_COLOR_FILTER_BAYER_BG: return "BG";
        case GX_COLOR_FILTER_NONE: return "NONE";
        default: return "UNKNOWN";
    }
}

// PixelColorFilter 节点的取值与 DxImageProc 的 DX_PIXEL_COLOR_FILTER 一一对应
// （NONE=0, BAYERRG=1, BAYERGB=2, BAYERGR=3, BAYERBG=4），但仍然显式映射一次，
// 不直接强转——两套枚举将来各自演进时，这里会编译报错而不是静默错色。
DX_PIXEL_COLOR_FILTER ToDxFilter(int64_t filter) {
    switch (filter) {
        case GX_COLOR_FILTER_BAYER_RG: return BAYERRG;
        case GX_COLOR_FILTER_BAYER_GB: return BAYERGB;
        case GX_COLOR_FILTER_BAYER_GR: return BAYERGR;
        case GX_COLOR_FILTER_BAYER_BG: return BAYERBG;
        default: return NONE;
    }
}

bool IsBayer8(int64_t format) {
    return format == GX_PIXEL_FORMAT_BAYER_RG8 || format == GX_PIXEL_FORMAT_BAYER_GB8 ||
           format == GX_PIXEL_FORMAT_BAYER_GR8 || format == GX_PIXEL_FORMAT_BAYER_BG8;
}

}  // namespace

// ---------------------------------------------------------------------------
// 配置转字符串：启动时打一行日志，出问题时对着日志就能复原现场。
// ---------------------------------------------------------------------------
std::string ToString(const CameraConfig &c) {
    std::ostringstream os;
    os << "exposure=" << c.exposure_time_us << "us"
       << " gain=" << c.gain_db << "dB"
       << " gamma=" << c.gamma
       << " auto[exposure=" << (c.auto_exposure ? "on" : "off")
       << ",gain=" << (c.auto_gain ? "on" : "off")
       << ",awb=" << (c.auto_white_balance ? "on" : "off") << "]"
       << " pixel_format=" << c.pixel_format
       << " roi=" << (c.width > 0 ? std::to_string(c.width) : std::string("full")) << "x"
       << (c.height > 0 ? std::to_string(c.height) : std::string("full"))
       << " acq_mode=" << c.acquisition_mode
       << " trigger=" << (c.trigger_off ? "off" : "on")
       << " throughput=" << (c.maximize_throughput ? "max" : "keep")
       << " buffer_num=" << c.acquisition_buffer_num
       << " frame_timeout=" << c.frame_timeout_ms << "ms";
    return os.str();
}

// ---------------------------------------------------------------------------
// 库级接口
// ---------------------------------------------------------------------------
bool DahengCamera::InitLib() {
    GX_STATUS status = GXInitLib();  // 大恒必须先做一次全局初始化，海康无此步骤
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} GXInitLib 失败: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                     ErrorString(status));
        return false;
    }
    RM_LOG_INFO("{} GalaxySDK 初始化完成 (GXInitLib)", kTag);
    return true;
}

void DahengCamera::CloseLib() {
    GX_STATUS status = GXCloseLib();  // 与 GXInitLib 配对
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_WARN("{} GXCloseLib 返回异常: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                    ErrorString(status));
    } else {
        RM_LOG_INFO("{} GalaxySDK 已释放 (GXCloseLib)", kTag);
    }
}

std::string DahengCamera::ErrorString(GX_STATUS status) {
    // 大恒 SDK 可以直接把错误原因问出来（对比海康只能拿错误码自己查表）：
    // 第一次调用只取长度，第二次才取内容。
    size_t size = 0;
    if (GXGetLastError(&status, nullptr, &size) != GX_STATUS_SUCCESS) {
        return "<GXGetLastError 取长度失败>";
    }
    std::vector<char> text(size + 1, '\0');
    if (GXGetLastError(&status, text.data(), &size) != GX_STATUS_SUCCESS) {
        return "<GXGetLastError 取内容失败>";
    }
    text[size] = '\0';
    return std::string(text.data());
}

std::vector<DeviceInfo> DahengCamera::Enumerate(uint32_t timeout_ms) {
    std::vector<DeviceInfo> devices;

    uint32_t device_num = 0;
    GX_STATUS status = GXUpdateAllDeviceList(&device_num, timeout_ms);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 枚举设备失败: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                     ErrorString(status));
        return devices;
    }
    RM_LOG_INFO("{} 枚举到 {} 台设备 (超时 {} ms)", kTag, device_num, timeout_ms);
    if (device_num == 0) {
        RM_LOG_WARN("{} 未发现相机，请依次检查: 1) 相机上电 2) USB3.0 接口(蓝色口) "
                    "3) GalaxySDK 驱动是否安装 4) Linux 下 udev 权限(Galaxy_camera.rules)",
                    kTag);
        return devices;
    }

    for (uint32_t i = 1; i <= device_num; ++i) {  // 序号从 1 开始！海康是 0 开始
        GX_DEVICE_INFO info;
        std::memset(&info, 0, sizeof(info));
        status = GXGetDeviceInfo(i, &info);
        if (status != GX_STATUS_SUCCESS) {
            RM_LOG_WARN("{} 读取 {} 号设备信息失败: [0x{:08X}] {}", kTag, i,
                        static_cast<uint32_t>(status), ErrorString(status));
            continue;
        }

        DeviceInfo device;
        device.index = i;
        device.device_class = DeviceClassName(info.emDevType);
        switch (info.emDevType) {
            case GX_DEVICE_CLASS_U3V:
                device.vendor = FixedString(info.DevInfo.stU3VDevInfo.chVendorName);
                device.model = FixedString(info.DevInfo.stU3VDevInfo.chModelName);
                device.serial = FixedString(info.DevInfo.stU3VDevInfo.chSerialNumber);
                device.user_id = FixedString(info.DevInfo.stU3VDevInfo.chUserDefinedName);
                break;
            case GX_DEVICE_CLASS_GEV:
                device.vendor = FixedString(info.DevInfo.stGEVDevInfo.chVendorName);
                device.model = FixedString(info.DevInfo.stGEVDevInfo.chModelName);
                device.serial = FixedString(info.DevInfo.stGEVDevInfo.chSerialNumber);
                device.user_id = FixedString(info.DevInfo.stGEVDevInfo.chUserDefinedName);
                break;
            case GX_DEVICE_CLASS_USB2:
                device.vendor = FixedString(info.DevInfo.stUSBDevInfo.chVendorName);
                device.model = FixedString(info.DevInfo.stUSBDevInfo.chModelName);
                device.serial = FixedString(info.DevInfo.stUSBDevInfo.chSerialNumber);
                device.user_id = FixedString(info.DevInfo.stUSBDevInfo.chUserDefinedName);
                break;
            default:
                device.vendor = "unknown";
                device.model = "unknown";
                break;
        }

        RM_LOG_INFO("{}   [{}] {} | 厂商: {} | 型号: {} | 序列号: {}", kTag, i, device.device_class,
                    device.vendor, device.model, device.serial);
        devices.push_back(device);
    }
    return devices;
}

// ---------------------------------------------------------------------------
// 打开 / 关闭
// ---------------------------------------------------------------------------
DahengCamera::~DahengCamera() {
    // 析构必须保证逆序收尾：停流 -> 关设备。忘关设备会让下一轮 GXOpenDevice
    // 报"设备已被占用"，这类问题在日志里会表现为第二次启动必然失败。
    Close();
}

bool DahengCamera::OpenBySerial(const std::string &serial) {
    if (IsOpen()) {
        RM_LOG_WARN("{} 相机已经打开，忽略重复打开请求", kTag);
        return true;
    }
    if (serial.empty()) {
        RM_LOG_ERROR("{} 序列号为空，无法打开", kTag);
        return false;
    }

    // 用现代接口 GXOpenDevice（培训 demo 里的 GXOpenDeviceByIndex 属于 legacy 接口，
    // 现版本 SDK 保留但它不支持按序列号打开、也不好控制访问模式）。
    GX_OPEN_PARAM param;
    std::memset(&param, 0, sizeof(param));
    param.pszContent = const_cast<char *>(serial.c_str());
    param.openMode = GX_OPEN_SN;
    param.accessMode = GX_ACCESS_EXCLUSIVE;  // 独占：别人再抢会立刻报错，而不是静默抢帧

    GX_DEV_HANDLE handle = nullptr;
    GX_STATUS status = GXOpenDevice(&param, &handle);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 按序列号打开相机失败 SN={}: [0x{:08X}] {}", kTag, serial,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }

    device_ = handle;
    RM_LOG_INFO("{} 打开相机成功 SN={} (GX_ACCESS_EXCLUSIVE)", kTag, serial);
    LogDeviceInfo();
    return true;
}

bool DahengCamera::OpenByIndex(uint32_t index) {
    if (IsOpen()) {
        RM_LOG_WARN("{} 相机已经打开，忽略重复打开请求", kTag);
        return true;
    }
    const std::string content = std::to_string(index);

    GX_OPEN_PARAM param;
    std::memset(&param, 0, sizeof(param));
    param.pszContent = const_cast<char *>(content.c_str());
    param.openMode = GX_OPEN_INDEX;  // 大恒序号从 1 开始
    param.accessMode = GX_ACCESS_EXCLUSIVE;

    GX_DEV_HANDLE handle = nullptr;
    GX_STATUS status = GXOpenDevice(&param, &handle);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 按序号打开相机失败 index={}: [0x{:08X}] {}", kTag, index,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }

    device_ = handle;
    RM_LOG_INFO("{} 打开相机成功 index={} (GX_ACCESS_EXCLUSIVE)", kTag, index);
    LogDeviceInfo();
    return true;
}

void DahengCamera::Close() {
    if (streaming_) {
        Stop();
    }
    if (device_ != nullptr) {
        GX_STATUS status = GXCloseDevice(device_);
        if (status != GX_STATUS_SUCCESS) {
            RM_LOG_WARN("{} GXCloseDevice 返回异常: [0x{:08X}] {}", kTag,
                        static_cast<uint32_t>(status), ErrorString(status));
        } else {
            RM_LOG_INFO("{} 相机已关闭，累计收帧 {} 帧 / 丢帧 {} / 超时 {}", kTag,
                        received_frames_, dropped_frames_, timeout_count_);
        }
        device_ = nullptr;
    }
    stream_ = nullptr;
    payload_size_ = 0;
    width_ = 0;
    height_ = 0;
}

// ---------------------------------------------------------------------------
// 节点读写小工具
// ---------------------------------------------------------------------------
bool DahengCamera::ReadFloatNode(const char *node, double &value) const {
    if (device_ == nullptr) {
        return false;
    }
    GX_FLOAT_VALUE float_value;
    std::memset(&float_value, 0, sizeof(float_value));
    GX_STATUS status = GXGetFloatValue(device_, node, &float_value);
    if (status != GX_STATUS_SUCCESS) {
        return false;
    }
    value = float_value.dCurValue;
    return true;
}

bool DahengCamera::SetFloatNode(const char *node, double value, const char *unit) {
    GX_FLOAT_VALUE float_value;
    std::memset(&float_value, 0, sizeof(float_value));
    GX_STATUS status = GXGetFloatValue(device_, node, &float_value);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 读取节点 {} 失败: [0x{:08X}] {}", kTag, node,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }

    // 限幅：相机只接受 [dMin, dMax] 区间内的值，超范围会直接报错而不是自动截断。
    const double clamped = std::min(std::max(value, float_value.dMin), float_value.dMax);
    if (clamped != value) {
        RM_LOG_WARN("{} {}={} 超出相机允许范围 [{}, {}]，已限幅为 {}", kTag, node, value,
                    float_value.dMin, float_value.dMax, clamped);
    }

    status = GXSetFloatValue(device_, node, clamped);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 设置节点 {}={} 失败: [0x{:08X}] {}", kTag, node, clamped,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }

    double readback = 0.0;
    if (ReadFloatNode(node, readback)) {
        RM_LOG_INFO("{} {} -> {:.3f}{} (相机回读 {:.3f}{}, 范围 [{}, {}])", kTag, node, clamped,
                    unit, readback, unit, float_value.dMin, float_value.dMax);
    } else {
        RM_LOG_INFO("{} {} -> {:.3f}{}", kTag, node, clamped, unit);
    }
    return true;
}

bool DahengCamera::SetIntNode(const char *node, int64_t value) {
    GX_INT_VALUE int_value;
    std::memset(&int_value, 0, sizeof(int_value));
    GX_STATUS status = GXGetIntValue(device_, node, &int_value);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 读取节点 {} 失败: [0x{:08X}] {}", kTag, node,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }

    int64_t clamped = std::min(std::max(value, int_value.nMin), int_value.nMax);
    // 步进对齐：Width/Height 这类节点有 nInc（常见为 4/8/16），不按步进对齐会被拒。
    if (int_value.nInc > 1) {
        clamped = int_value.nMin + ((clamped - int_value.nMin) / int_value.nInc) * int_value.nInc;
    }

    status = GXSetIntValue(device_, node, clamped);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} 设置节点 {}={} 失败: [0x{:08X}] {}", kTag, node, clamped,
                     static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }
    RM_LOG_INFO("{} {} -> {} (范围 [{}, {}], 步进 {})", kTag, node, clamped, int_value.nMin,
                int_value.nMax, int_value.nInc);
    return true;
}

bool DahengCamera::TrySetEnumByString(const char *node, const char *value, bool required) {
    GX_STATUS status = GXSetEnumValueByString(device_, node, value);
    if (status == GX_STATUS_SUCCESS) {
        RM_LOG_DEBUG("{} 节点 {} := {}", kTag, node, value);
        return true;
    }
    if (required) {
        RM_LOG_ERROR("{} 设置节点 {}={} 失败: [0x{:08X}] {}", kTag, node, value,
                     static_cast<uint32_t>(status), ErrorString(status));
    } else {
        // 可选节点：黑白相机没有白平衡，不同型号没有伽马，都属于正常情况。
        RM_LOG_DEBUG("{} 可选节点 {}={} 不可用: [0x{:08X}] {}", kTag, node, value,
                     static_cast<uint32_t>(status), ErrorString(status));
    }
    return false;
}

bool DahengCamera::LogEnumNode(const char *node) const {
    auto enum_value = std::make_unique<GX_ENUM_VALUE>();
    std::memset(enum_value.get(), 0, sizeof(GX_ENUM_VALUE));
    GX_STATUS status = GXGetEnumValue(device_, node, enum_value.get());
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_DEBUG("{} 节点 {} 不可读: [0x{:08X}] {}", kTag, node, static_cast<uint32_t>(status),
                     ErrorString(status));
        return false;
    }

    // SDK 会把该节点的全部合法取值一起返回，打出来等于免费拿到一份能力清单，
    // 排查"这台相机支持哪些像素格式"时不用再翻文档。
    std::string supported;
    const uint32_t count = std::min<uint32_t>(enum_value->nSupportedNum, 128);
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) {
            supported += " | ";
        }
        supported += Symbolic(enum_value->nArrySupportedValue[i]);
    }
    RM_LOG_DEBUG("{} {} = {} ({}), 可选值: {}", kTag, node, Symbolic(enum_value->stCurValue),
                 enum_value->stCurValue.nCurValue, supported);
    return true;
}

bool DahengCamera::LogStringNode(const char *node) const {
    GX_STRING_VALUE string_value;
    std::memset(&string_value, 0, sizeof(string_value));
    GX_STATUS status = GXGetStringValue(device_, node, &string_value);
    if (status != GX_STATUS_SUCCESS) {
        return false;
    }
    RM_LOG_INFO("{}   {} = {}", kTag, node, string_value.strCurValue);
    return true;
}

void DahengCamera::LogDeviceInfo() const {
    // 这些字符串节点在设备打开后才可读，能一次性确认"连上的到底是哪台相机"。
    LogStringNode("DeviceVendorName");
    LogStringNode("DeviceModelName");
    LogStringNode("DeviceSerialNumber");
    LogStringNode("DeviceVersion");
    LogStringNode("DeviceFirmwareVersion");
    LogEnumNode("PixelFormat");
}

// ---------------------------------------------------------------------------
// 配置下发
// ---------------------------------------------------------------------------
bool DahengCamera::Configure(const CameraConfig &config) {
    if (device_ == nullptr) {
        RM_LOG_ERROR("{} 配置前必须先用 OpenBySerial/OpenByIndex 打开相机", kTag);
        return false;
    }

    config_ = config;
    RM_LOG_INFO("{} 开始配置相机: {}", kTag, ToString(config_));

    // 1) 先关自动挡。顺序很重要：先自动后手动，手动值会被自动算法覆盖；
    //    先手写曝光再关自动，同样可能被自动算法改回去。
    TrySetEnumByString("ExposureAuto", config_.auto_exposure ? "Continuous" : "Off", true);
    TrySetEnumByString("GainAuto", config_.auto_gain ? "Continuous" : "Off", true);
    TrySetEnumByString("BalanceWhiteAuto", config_.auto_white_balance ? "Continuous" : "Off",
                       false);

    // 2) 连续采集 + 关触发：软件取流用触发的话，拿不到触发源就会一直 DQBuf 超时。
    TrySetEnumByString("AcquisitionMode", config_.acquisition_mode.c_str(), true);
    if (config_.trigger_off) {
        TrySetEnumByString("TriggerMode", "Off", true);
    }

    // 3) 像素格式。用符号名下发的意义是：同一份代码换台相机也能跑，不写死魔法数字。
    if (!TrySetEnumByString("PixelFormat", config_.pixel_format.c_str(), true)) {
        RM_LOG_ERROR("{} PixelFormat={} 下发失败，请用 --list 看该型号支持哪些格式", kTag,
                     config_.pixel_format);
        return false;
    }

    // 4) ROI。像素格式变了以后 Width/Height 的可选范围也会变，所以放在第 3 步之后。
    if (config_.width > 0 || config_.height > 0) {
        // 改幅面时要先把偏移归零，否则 Width 会顶出传感器边界而报错。
        if (config_.width > 0) {
            SetIntNode("OffsetX", 0);
            SetIntNode("Width", config_.width);
        }
        if (config_.height > 0) {
            SetIntNode("OffsetY", 0);
            SetIntNode("Height", config_.height);
        }
    }

    // 5) 读回真实分辨率（全幅时就是相机当前值）。
    GX_INT_VALUE int_value;
    std::memset(&int_value, 0, sizeof(int_value));
    if (GXGetIntValue(device_, "Width", &int_value) == GX_STATUS_SUCCESS) {
        width_ = static_cast<int>(int_value.nCurValue);
    }
    if (GXGetIntValue(device_, "Height", &int_value) == GX_STATUS_SUCCESS) {
        height_ = static_cast<int>(int_value.nCurValue);
    }
    if (width_ <= 0 || height_ <= 0) {
        RM_LOG_ERROR("{} 分辨率读取失败 Width={} Height={}，无法继续", kTag, width_, height_);
        return false;
    }
    RM_LOG_INFO("{} 分辨率 {}x{}", kTag, width_, height_);

    // 6) 曝光/增益/伽马
    SetFloatNode("ExposureTime", config_.exposure_time_us, "us");
    SetFloatNode("Gain", config_.gain_db, "dB");
    if (config_.gamma >= 0.0) {
        SetFloatNode("Gamma", config_.gamma, "");
    } else {
        RM_LOG_DEBUG("{} Gamma 未指定，保持相机当前值", kTag);
    }

    // 7) 链路吞吐量。大恒 USB3 相机出厂限速，不拉满的话帧率只有理论值的几分之一，
    //    这是"帧率上不去"的头号原因，也是本项目相对培训 demo 必须复现的关键配置。
    std::memset(&int_value, 0, sizeof(int_value));
    if (GXGetIntValue(device_, "DeviceLinkThroughputLimit", &int_value) == GX_STATUS_SUCCESS) {
        RM_LOG_INFO("{} DeviceLinkThroughputLimit 当前 {} B/s, 范围 [{}, {}]", kTag,
                    int_value.nCurValue, int_value.nMin, int_value.nMax);
        if (config_.maximize_throughput) {
            SetIntNode("DeviceLinkThroughputLimit", int_value.nMax);
        }
    } else {
        RM_LOG_DEBUG("{} 该型号无 DeviceLinkThroughputLimit 节点，跳过（非 USB 相机常见）", kTag);
    }

    // 8) 数据流句柄 + 单帧字节数。大恒的"流"是独立句柄，payload 要从流上查。
    uint32_t stream_num = 0;
    GX_STATUS status = GXGetDataStreamNumFromDev(device_, &stream_num);
    if (status != GX_STATUS_SUCCESS || stream_num < config_.stream_index) {
        RM_LOG_ERROR("{} 获取数据流失败: 流数={} 需要序号={} [0x{:08X}] {}", kTag, stream_num,
                     config_.stream_index, static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }
    status = GXGetDataStreamHandleFromDev(device_, config_.stream_index, &stream_);
    if (status != GX_STATUS_SUCCESS || stream_ == nullptr) {
        RM_LOG_ERROR("{} GXGetDataStreamHandleFromDev({}) 失败: [0x{:08X}] {}", kTag,
                     config_.stream_index, static_cast<uint32_t>(status), ErrorString(status));
        return false;
    }
    status = GXGetPayLoadSize(stream_, &payload_size_);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_WARN("{} GXGetPayLoadSize 失败: [0x{:08X}] {}", kTag,
                    static_cast<uint32_t>(status), ErrorString(status));
        payload_size_ = 0;
    } else {
        RM_LOG_INFO("{} 数据流 {} 就绪, payload = {} 字节/帧", kTag, config_.stream_index,
                    payload_size_);
    }

    // 9) SDK 内部取流缓冲个数：越大越抗抖动，但每块都占内存（payload * num）。
    status = GXSetAcqusitionBufferNumber(device_, config_.acquisition_buffer_num);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_WARN("{} GXSetAcqusitionBufferNumber({}) 失败: [0x{:08X}] {}", kTag,
                    config_.acquisition_buffer_num, static_cast<uint32_t>(status),
                    ErrorString(status));
    } else {
        RM_LOG_INFO("{} 取流缓冲个数 = {}", kTag, config_.acquisition_buffer_num);
    }

    // 10) Bayer 排列。转换函数必须收到正确的排列，否则颜色整体错乱（红蓝互换等）。
    color_filter_ = GX_COLOR_FILTER_NONE;
    bayer_filter_ = NONE;
    GX_ENUM_VALUE filter_value;
    std::memset(&filter_value, 0, sizeof(filter_value));
    status = GXGetEnumValue(device_, "PixelColorFilter", &filter_value);
    if (status == GX_STATUS_SUCCESS) {
        color_filter_ = filter_value.stCurValue.nCurValue;
        bayer_filter_ = ToDxFilter(color_filter_);
        RM_LOG_INFO("{} PixelColorFilter = {} -> DX_PIXEL_COLOR_FILTER({})", kTag,
                    ColorFilterName(color_filter_), static_cast<int>(bayer_filter_));
    } else {
        RM_LOG_DEBUG("{} 无 PixelColorFilter 节点（黑白相机），按 Mono 处理", kTag);
    }

    // 11) 转换缓冲区按最大幅面预分配，运行期不再 malloc。
    const size_t buffer_size =
        static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3U;
    rgb_buffer_.assign(buffer_size, 0);
    RM_LOG_INFO("{} 转换缓冲区 {} 字节 ({})", kTag, buffer_size, "Bayer/RAW -> BGR24");

    RM_LOG_INFO("{} 相机配置完成", kTag);
    return true;
}

// ---------------------------------------------------------------------------
// 取流
// ---------------------------------------------------------------------------
bool DahengCamera::Start() {
    if (device_ == nullptr || stream_ == nullptr) {
        RM_LOG_ERROR("{} 取流前必须先 Open 并 Configure", kTag);
        return false;
    }
    if (streaming_) {
        RM_LOG_WARN("{} 已在取流中，忽略重复 Start", kTag);
        return true;
    }

    GX_STATUS status = GXStreamOn(device_);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} GXStreamOn 失败: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                     ErrorString(status));
        return false;
    }

    streaming_ = true;
    received_frames_ = 0;
    dropped_frames_ = 0;
    timeout_count_ = 0;
    last_frame_id_ = -1;

    // 上一次取流残留的旧帧会以"时间戳很旧但状态正常"的形式混进来，先清干净。
    GXFlushQueue(device_);

    double exposure = 0.0;
    double gain = 0.0;
    ReadExposureTime(exposure);
    ReadGain(gain);
    RM_LOG_INFO("{} 取流已开始 (GXStreamOn) 曝光={:.3f}us 增益={:.3f}dB", kTag, exposure, gain);
    return true;
}

bool DahengCamera::Stop() {
    if (!streaming_) {
        return true;
    }
    streaming_ = false;

    GX_STATUS status = GXStreamOff(device_);
    if (status != GX_STATUS_SUCCESS) {
        RM_LOG_WARN("{} GXStreamOff 返回异常: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                    ErrorString(status));
    } else {
        RM_LOG_INFO("{} 取流已停止 (GXStreamOff)", kTag);
    }
    return status == GX_STATUS_SUCCESS;
}

bool DahengCamera::Grab(cv::Mat &bgr) {
    if (!streaming_ || device_ == nullptr) {
        RM_LOG_ERROR("{} 尚未开始取流，无法取帧", kTag);
        return false;
    }

    PGX_FRAME_BUFFER frame = nullptr;
    GX_STATUS status = GXDQBuf(device_, &frame, static_cast<uint32_t>(config_.frame_timeout_ms));
    if (status != GX_STATUS_SUCCESS || frame == nullptr) {
        if (status == GX_STATUS_TIMEOUT) {
            ++timeout_count_;
            RM_LOG_WARN("{} GXDQBuf 超时 {} ms（累计 {} 次）: [0x{:08X}] {}", kTag,
                        config_.frame_timeout_ms, timeout_count_, static_cast<uint32_t>(status),
                        ErrorString(status));
        } else {
            RM_LOG_ERROR("{} GXDQBuf 失败: [0x{:08X}] {}", kTag, static_cast<uint32_t>(status),
                         ErrorString(status));
        }
        return false;
    }

    ++received_frames_;
    last_frame_id_ = static_cast<int64_t>(frame->nFrameID);

    bool ok = false;
    if (frame->nStatus != GX_FRAME_STATUS_SUCCESS) {
        // 丢包 / 传输不完整的帧：日志记一笔，然后照常归还缓冲。
        ++dropped_frames_;
        RM_LOG_WARN("{} 帧状态异常 frame_id={} status=0x{:08X}（累计丢帧 {}）", kTag,
                    frame->nFrameID, static_cast<uint32_t>(frame->nStatus), dropped_frames_);
    } else {
        ok = ConvertFrameToBgr(frame, bgr);
    }

    // 无论帧好坏都必须归还（QB = EnQueue）。漏还的症状是队列被掏空、
    // GXDQBuf 永远超时、帧率掉到 0——这类 bug 最难查，所以归还写在唯一出口。
    GX_STATUS qb_status = GXQBuf(device_, frame);
    if (qb_status != GX_STATUS_SUCCESS) {
        RM_LOG_ERROR("{} GXQBuf 归还缓冲失败: [0x{:08X}] {}（继续下去会耗尽缓冲队列）", kTag,
                     static_cast<uint32_t>(qb_status), ErrorString(qb_status));
    }
    return ok;
}

bool DahengCamera::ConvertFrameToBgr(const GX_FRAME_BUFFER *frame, cv::Mat &bgr) {
    const int width = frame->nWidth;
    const int height = frame->nHeight;
    const int64_t format = frame->nPixelFormat;
    if (width <= 0 || height <= 0 || frame->pImgBuf == nullptr) {
        RM_LOG_ERROR("{} 帧数据非法: {}x{} buf={}", kTag, width, height,
                     frame->pImgBuf == nullptr ? "null" : "ok");
        return false;
    }

    if (IsBayer8(format)) {
        const size_t needed = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
        if (rgb_buffer_.size() < needed) {
            RM_LOG_WARN("{} 转换缓冲区不足({} < {})，按当前帧扩容", kTag, rgb_buffer_.size(), needed);
            rgb_buffer_.assign(needed, 0);
        }
        // 参数含义: 输入 RAW -> 输出 BGR24，邻域插值，Bayer 排列来自 PixelColorFilter，
        //           不翻转，通道序为 OpenCV 的 BGR。
        VxInt32 dx_status =
            DxRaw8toRGB24Ex(frame->pImgBuf, rgb_buffer_.data(), static_cast<VxUint32>(width),
                            static_cast<VxUint32>(height), RAW2RGB_NEIGHBOUR, bayer_filter_, false,
                            DX_ORDER_BGR);
        if (dx_status != DX_OK) {
            RM_LOG_ERROR("{} DxRaw8toRGB24Ex 失败: 0x{:08X}（Bayer{} {}x{}）", kTag,
                         static_cast<uint32_t>(dx_status), ColorFilterName(color_filter_), width,
                         height);
            return false;
        }
        // 拷贝出去：rgb_buffer_ 是复用的，直接包 Mat 会在下一帧被覆盖。
        cv::Mat(height, width, CV_8UC3, rgb_buffer_.data()).copyTo(bgr);
        return true;
    }

    if (format == GX_PIXEL_FORMAT_MONO8) {
        cv::Mat gray(height, width, CV_8UC1, frame->pImgBuf);
        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
        return true;
    }
    if (format == GX_PIXEL_FORMAT_BGR8) {
        cv::Mat view(height, width, CV_8UC3, frame->pImgBuf);
        view.copyTo(bgr);
        return true;
    }
    if (format == GX_PIXEL_FORMAT_RGB8) {
        cv::Mat view(height, width, CV_8UC3, frame->pImgBuf);
        cv::cvtColor(view, bgr, cv::COLOR_RGB2BGR);
        return true;
    }

    // Mono10/12、Bayer10/12 等高位深格式需要先做位深变换，超出本项目范围：
    // 只提示一次，避免每帧刷屏把日志冲爆。
    if (!unsupported_format_logged_) {
        unsupported_format_logged_ = true;
        RM_LOG_ERROR("{} 暂不支持的像素格式 {} (0x{:08X})，请把 PixelFormat 设为 "
                     "BayerRG8 / Mono8 / RGB8 / BGR8",
                     kTag, PixelFormatName(format), static_cast<uint32_t>(format));
    }
    return false;
}

// ---------------------------------------------------------------------------
// 运行期调参
// ---------------------------------------------------------------------------
bool DahengCamera::SetExposureTime(double us) {
    if (!SetFloatNode("ExposureTime", us, "us")) {
        return false;
    }
    config_.exposure_time_us = us;
    return true;
}

bool DahengCamera::SetGain(double db) {
    if (!SetFloatNode("Gain", db, "dB")) {
        return false;
    }
    config_.gain_db = db;
    return true;
}

bool DahengCamera::SetGamma(double gamma) {
    if (!SetFloatNode("Gamma", gamma, "")) {
        return false;
    }
    config_.gamma = gamma;
    return true;
}

bool DahengCamera::ReadExposureTime(double &us) const { return ReadFloatNode("ExposureTime", us); }

bool DahengCamera::ReadGain(double &db) const { return ReadFloatNode("Gain", db); }

bool DahengCamera::ReadGamma(double &gamma) const { return ReadFloatNode("Gamma", gamma); }

}  // namespace camera
