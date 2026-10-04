// 大恒（Daheng Imaging）工业相机封装。
//
// 把培训 demo 里"一坨 main"的流程收成一个类，职责边界：
//   库生命周期  InitLib / CloseLib   （大恒特有：必须先全局初始化，海康没有这一步）
//   设备发现    Enumerate
//   打开关闭    OpenBySerial / OpenByIndex / Close
//   参数下发    Configure / SetExposureTime / SetGain / SetGamma ...
//   取流显示    Start / Stop / Grab（Grab 直接给出一帧 BGR 的 cv::Mat）
//
// 所有调试信息统一走 rm_log 的 RM_LOG_* 宏，本文件不出现任何 std::cout。
#ifndef DAHENG_CAMERA_DAHENG_CAMERA_H_
#define DAHENG_CAMERA_DAHENG_CAMERA_H_

#include "camera/camera_config.h"

#include <sdk/DxImageProc.h>
#include <sdk/GxIAPI.h>

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace camera {

// 一台被枚举到的设备。大恒用"1 开始的序号"定位设备（海康是 0 开始，这是两套 SDK
// 最容易踩的差异），所以 index 字段保留原始序号，序列号用于精确匹配。
struct DeviceInfo {
    uint32_t    index = 0;      // 大恒设备序号，从 1 开始
    std::string device_class;   // U3V(USB3.0) / GEV(GigE) / USB2.0 ...
    std::string vendor;
    std::string model;
    std::string serial;
    std::string user_id;        // UserDefinedName，多相机场景可以用它起别名
};

class DahengCamera {
public:
    DahengCamera() = default;
    ~DahengCamera();

    // 持有 SDK 句柄，禁止拷贝；需要转移时用指针或 std::unique_ptr。
    DahengCamera(const DahengCamera &) = delete;
    DahengCamera &operator=(const DahengCamera &) = delete;

    // ---------------- 库级接口（进程内只需一次） ----------------
    static bool InitLib();
    static void CloseLib();

    // 枚举设备。超时给足，GigE 相机响应慢，官方建议 >= 1000 ms。
    static std::vector<DeviceInfo> Enumerate(uint32_t timeout_ms = 1000);

    // GX_STATUS -> 人类可读字符串（大恒 SDK 自带 GXGetLastError，比查错误码表省事）。
    static std::string ErrorString(GX_STATUS status);

    // ---------------- 设备级接口 ----------------
    bool OpenBySerial(const std::string &serial);
    bool OpenByIndex(uint32_t index);

    // 下发配置。要求已 Open，且必须在 Start 之前调用。
    bool Configure(const CameraConfig &config);

    bool Start();
    bool Stop();
    void Close();

    // 取一帧，转成 BGR 的 cv::Mat。返回 false 时 bgr 内容无意义（超时/坏帧/格式不支持）。
    bool Grab(cv::Mat &bgr);

    // ---------------- 运行期调参 ----------------
    bool SetExposureTime(double us);
    bool SetGain(double db);
    bool SetGamma(double gamma);
    bool ReadExposureTime(double &us) const;
    bool ReadGain(double &db) const;
    bool ReadGamma(double &gamma) const;

    // ---------------- 状态查询 ----------------
    bool     IsOpen() const { return device_ != nullptr; }
    bool     IsStreaming() const { return streaming_; }
    int      Width() const { return width_; }
    int      Height() const { return height_; }
    uint32_t PayloadSize() const { return payload_size_; }
    uint64_t ReceivedFrames() const { return received_frames_; }
    uint64_t DroppedFrames() const { return dropped_frames_; }
    uint64_t TimeoutCount() const { return timeout_count_; }
    int64_t  LastFrameId() const { return last_frame_id_; }
    const CameraConfig &Config() const { return config_; }

private:
    // 把 SDK 帧缓冲转成 BGR。内部按帧头里的 nPixelFormat 分派，不看配置——
    // 相机实际吐什么格式，就以什么格式解，避免配置与实况不一致时花屏。
    bool ConvertFrameToBgr(const GX_FRAME_BUFFER *buffer, cv::Mat &bgr);

    // 通用节点读写：读-改-限幅-写，失败一律带错误码落日志。
    bool SetFloatNode(const char *node, double value, const char *unit);
    bool ReadFloatNode(const char *node, double &value) const;
    bool SetIntNode(const char *node, int64_t value);
    bool TrySetEnumByString(const char *node, const char *value, bool required);
    bool LogEnumNode(const char *node) const;   // 打印当前值 + 全部可选值
    bool LogStringNode(const char *node) const;  // 打印 DeviceModelName 之类的字符串节点
    void LogDeviceInfo() const;

    GX_DEV_HANDLE device_ = nullptr;
    GX_DS_HANDLE  stream_ = nullptr;
    bool          streaming_ = false;

    CameraConfig config_;
    int          width_ = 0;
    int          height_ = 0;
    uint32_t     payload_size_ = 0;

    // Bayer 排列：必须先查相机的 PixelColorFilter 节点，否则转换出来的颜色是错的。
    int64_t               color_filter_ = GX_COLOR_FILTER_NONE;
    DX_PIXEL_COLOR_FILTER bayer_filter_ = NONE;

    // 转换缓冲区，按需增长，避免每帧 malloc。
    std::vector<unsigned char> rgb_buffer_;

    uint64_t received_frames_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t timeout_count_ = 0;
    int64_t  last_frame_id_ = -1;
    bool     unsupported_format_logged_ = false;
};

}  // namespace camera

#endif  // DAHENG_CAMERA_DAHENG_CAMERA_H_
