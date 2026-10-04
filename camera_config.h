// 相机工作参数集合。
//
// 每一项都对应大恒 GalaxySDK 的一个 GenICam 节点名，Configure() 时逐项下发。
// 单独抽成结构体，是为了让"配置"成为可读、可打印、可命令行覆盖的数据，
// 而不是散落在取流代码里的魔法数字。
#ifndef DAHENG_CAMERA_CAMERA_CONFIG_H_
#define DAHENG_CAMERA_CAMERA_CONFIG_H_

#include <cstdint>
#include <string>

namespace camera {

struct CameraConfig {
    // ---------------- 曝光 / 增益 / 伽马 ----------------
    // 注意：调参前必须先关自动挡，否则写进去的曝光值会被自动算法立刻覆盖。
    bool   auto_exposure = false;            // ExposureAuto     := Off
    bool   auto_gain = false;                // GainAuto         := Off
    bool   auto_white_balance = true;        // BalanceWhiteAuto := Continuous
    double exposure_time_us = 8000.0;        // ExposureTime     [us]
    double gain_db = 0.0;                    // Gain             [dB]
    double gamma = -1.0;                     // Gamma，<0 表示"不干预相机当前值"

    // ---------------- 采集模式 / 像素格式 ----------------
    std::string acquisition_mode = "Continuous";  // AcquisitionMode
    bool   trigger_off = true;                    // TriggerMode := Off
    std::string pixel_format = "BayerRG8";        // PixelFormat（符号名，交给 SDK 解析）
    int    width = 0;                             // 0 = 沿用相机当前宽度（全幅）
    int    height = 0;                            // 0 = 沿用相机当前高度（全幅）

    // ---------------- 流控 ----------------
    // 大恒 USB3 相机出厂带链路限速，不拉满的话帧率只有理论值的几分之一，
    // 这是"帧率上不去"的头号原因。
    bool     maximize_throughput = true;     // DeviceLinkThroughputLimit := nMax
    uint32_t acquisition_buffer_num = 5;     // GXSetAcqusitionBufferNumber
    int      frame_timeout_ms = 1000;        // GXDQBuf 超时
    uint32_t stream_index = 1;               // 大恒数据流序号从 1 开始
};

// 把配置压成一行，便于启动时用 RM_LOG_INFO 打出来存档。
std::string ToString(const CameraConfig &config);

}  // namespace camera

#endif  // DAHENG_CAMERA_CAMERA_CONFIG_H_
