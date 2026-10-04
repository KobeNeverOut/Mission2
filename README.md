# daheng_camera_rmlog

大恒（Daheng Imaging / GalaxySDK）工业相机取流显示工程：**枚举 → 按序列号打开 → 下发配置 → 取流 → 实时显示 → 运行期调参 → 优雅收尾**，
所有调试信息统一走 `rm_log`（spdlog 封装）输出，不出现任何 `std::cout` / `printf` 调试语句。

本项目是对培训材料 `培训/使用工业相机/daheng_demo.cpp`（相机部分）与 `培训/使用日志库/util/rm_log`（日志部分）的合并复现与工程化。

---

## 一、功能

| 能力 | 说明 |
|---|---|
| 设备枚举 | `GXUpdateAllDeviceList` + `GXGetDeviceInfo`，打印序号 / 接口类型 / 厂商 / 型号 / 序列号 |
| 打开相机 | 现代接口 `GXOpenDevice`：`GX_OPEN_SN`（按序列号）或 `GX_OPEN_INDEX`（按序号），`GX_ACCESS_EXCLUSIVE` 独占 |
| 参数下发 | 自动挡开关、曝光、增益、伽马、像素格式、ROI、链路吞吐量、取流缓冲数、数据流 payload、Bayer 排列 |
| 取流显示 | `GXDQBuf` → `DxRaw8toRGB24Ex` → `cv::Mat(BGR)` → `cv::imshow` 叠加 FPS |
| 像素格式自适应 | 按帧头 `nPixelFormat` 分派：BayerRG/GB/GR/BG8、Mono8、RGB8、BGR8；不支持的高位深格式只提示一次 |
| 运行期调参 | 窗口按键实时改曝光 / 增益 / 伽马，读-改-限幅-写，并回读相机实际生效值 |
| 存图 | `p` 键即时存图，或 `--save-every N` 每 N 帧自动存 PNG |
| 日志 | `RM_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` 六个级别，控制台彩色 + 文件滚动（5 MB × 3） |
| 无窗口模式 | `--no-display`，或在没有 `DISPLAY` / `WAYLAND_DISPLAY` 的机器上自动降级（适合做吞吐测试） |
| 稳健性 | 坏帧照常归还、连续失败自动判定掉线、Ctrl+C 走正常收尾、OpenCV 无 GUI 后端时自动降级 |

## 二、目录结构

```
daheng_camera_rmlog/
├── CMakeLists.txt                  # 三处依赖自动探测：OpenCV / Threads / GalaxySDK
├── include/
│   ├── camera/
│   │   ├── camera_config.h         # 相机参数结构体（可打印、可命令行覆盖）
│   │   └── daheng_camera.h         # 相机封装类 DahengCamera
│   └── sdk/                        # 大恒 GalaxySDK 头文件（随仓库自带，版本固定）
│       ├── GxIAPI.h  GxIAPILegacy.h  GXDef.h  GXErrorList.h
│       └── GxPixelFormat.h  DxImageProc.h
├── src/
│   ├── daheng_camera.cpp           # 枚举/打开/配置/取流/调参实现
│   └── main.cpp                    # 命令行入口 + 取流主循环 + 键盘交互
├── docs/sample_run.log             # 真实相机（MER-230-168U3C）跑出的完整 DEBUG 日志
└── util/
    ├── rm_log/                     # 培训版日志封装（宏 + 单例，原样保留）
    │   ├── include/rm_log.h
    │   └── src/rm_log.cpp
    └── spdlog/                     # spdlog 1.12.0（header-only，随仓库自带）
```

## 三、依赖与已验证环境

| 依赖 | 版本 / 位置 | 说明 |
|---|---|---|
| 编译器 | g++ 11.4 (Ubuntu 22.04) | C++17 |
| CMake | ≥ 3.16 | |
| OpenCV | 4.5.5（`find_package` 或 `pkg-config opencv4` 二选一） | 需要 core / imgproc / highgui / imgcodecs |
| 大恒 GalaxySDK | `libgxiapi.so`（本机默认 `/usr/lib/`） | 头文件用仓库自带的 `include/sdk` |
| spdlog | 1.12.0，仓库自带 `util/spdlog` | header-only，无需单独安装 |
| 实测相机 | 大恒 **MER-230-168U3C**（SN `NE0190010001`），1920×1200 U3V 彩色相机 | 支持 `BayerRG8` / `BayerRG10`，USB3.0 接口 |

> 相机库只依赖 `libgxiapi.so`：`DxRaw8toRGB24Ex`、`GXOpenDevice` 等符号都在这一个库里（已用 `nm -D` 确认），
> 所以**不需要额外链接 DxImageProc 库**。CMake 里仍保留了自动探测，万一某版本拆库也不用手改。

## 四、编译

```bash
cd daheng_camera_rmlog
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build/daheng_camera_demo`。

GalaxySDK 装在非默认位置时：

```bash
cmake -S . -B build -DGALAXY_SDK_ROOT=/path/to/GalaxySDK
```

## 五、运行

```bash
# 1) 先确认相机被识别（只枚举，不打开相机）
./build/daheng_camera_demo --list

# 2) 按序列号打开并显示（推荐：多相机场景唯一可靠的定位方式）
./build/daheng_camera_demo --serial <序列号>

# 3) 带参数启动
./build/daheng_camera_demo --serial <序列号> --exposure 5000 --gain 3.0 --pixel-format BayerRG8

# 4) 无窗口机器 / 只测吞吐
./build/daheng_camera_demo --serial <序列号> --no-display --frames 300

# 5) 每 30 帧存一张图
./build/daheng_camera_demo --serial <序列号> --save-every 30 --save-dir snapshots
```

### 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `-l, --list` | | 只枚举设备，列出序列号 / 型号 |
| `-s, --serial <SN>` | 第一台 | 按序列号打开 |
| `--index <n>` | | 按大恒设备序号打开（**从 1 开始**） |
| `-e, --exposure <us>` | 8000 | 曝光时间，微秒 |
| `--gain <dB>` | 0 | 增益 |
| `--gamma <值>` | 不干预 | 伽马，`<0` 表示保持相机当前值 |
| `--pixel-format <名>` | `BayerRG8` | 符号名：`BayerRG8` / `Mono8` / `RGB8` … |
| `--width/--height <px>` | 0（全幅） | ROI；设置时会先把 `OffsetX/Y` 归零 |
| `--buffer-num <n>` | 5 | SDK 取流缓冲个数 |
| `--timeout <ms>` | 1000 | 单帧取流超时 |
| `--no-max-throughput` | 关 | 不把 `DeviceLinkThroughputLimit` 拉满（默认拉满） |
| `-f, --frames <n>` | 0（不限） | 取 n 帧后自动退出 |
| `--no-display` | 关 | 不开窗口 |
| `--save-dir/--save-every` | `snapshots` / 0 | 存图目录 / 每 N 帧存一张 |
| `--log-file` | `logs/daheng_camera.log` | 日志文件 |
| `--log-level/--console-level/--file-level` | `debug/debug/info` | 三级日志过滤 |

### 运行中按键（窗口有焦点时）

| 键 | 作用 |
|---|---|
| `e` / `d` | 曝光 +25 / −25 us |
| `a` / `z` | 增益 +0.1 / −0.1 dB |
| `s` / `w` | 伽马 +0.1 / −0.1 |
| `p` | 立即存一张 PNG |
| `q` / `ESC` | 退出（走完整收尾流程） |

## 六、日志（rm_log / spdlog）

### 用法

```cpp
// main 一开始就必须初始化，否则 RM_LOG_* 宏内部解引用的 logger 还是空指针
INIT_LOG("logs/daheng_camera.log", "debug", "info", "debug");
//        ^文件路径              ^控制台级别  ^文件级别  ^总级别

RM_LOG_INFO("{} 分辨率 {}x{}", kTag, width_, height_);
RM_LOG_DEBUG("{} 帧 {} fps={:.1f} frame_id={}", kTag, index, fps, frame_id);
RM_LOG_WARN("{} GXDQBuf 超时 {} ms（累计 {} 次）", kTag, timeout_ms, count);
RM_LOG_ERROR("{} GXStreamOn 失败: [0x{:08X}] {}", kTag, status, ErrorString(status));

// 退出前必须 Close：异步日志要 flush 线程池，否则队列里的日志会丢
utils::RMLOG::instance().Close();
```

### 输出形态

```
[2026-10-04 17:28:43.205] [info] [thread 585] [main.cpp main:393] [main] 大恒工业相机取流程序启动
[2026-10-04 17:28:43.213] [error] [thread 585] [daheng_camera.cpp InitLib:124] [camera] GXInitLib 失败: [0xFFFFFFFF] {...}
```

pattern 为 `[时间] [级别] [线程] [文件 函数:行号] 正文`，`RM_LOG_BASE` 通过 `spdlog::source_loc` 自动带上文件/行号/函数，
所以**不需要在消息里手写文件名**；正文里的 `[main]` / `[camera]` 是模块标签，便于 `grep` 分流。

### 本项目的日志约定

- 每帧信息用 `DEBUG`，1 秒级的全帧状态、配置结果用 `INFO`，可恢复的异常用 `WARN`，失败用 `ERROR`，致命用 `CRITICAL`；
  这样默认 `--console-level debug` 能看到全过程，而 **日志文件默认只记 `info` 及以上**，不会被每帧 DEBUG 冲爆。
- 所有 SDK 错误码统一经 `DahengCamera::ErrorString()` 转成中文原因（大恒 `GXGetLastError` 支持两段式取文本，比查错误码表省事）。
- 唯一不走日志的输出是 `--help` 的用法文本和参数错误提示：它们是给 shell 看的产品输出，不是诊断信息。
- 日志文件用 `rotating_file_sink_mt`，单文件 5 MB、保留 3 个，`warn` 及以上立即 flush（不正常退出也能留下现场）。

## 七、复现的相机配置

对照培训 `daheng_demo.cpp`，逐项复现并在日志里留痕（`Configure()` 里的顺序是有意为之）：

| 步骤 | 节点 | 值 | 为什么这个顺序 |
|---|---|---|---|
| 1 | `ExposureAuto` / `GainAuto` / `BalanceWhiteAuto` | `Off` / `Off` / `Continuous` | **必须先关自动挡**，否则后面写的曝光/增益会被自动算法立刻覆盖 |
| 2 | `AcquisitionMode` / `TriggerMode` | `Continuous` / `Off` | 软件取流用触发又没接触发源，会一直 `GXDQBuf` 超时 |
| 3 | `PixelFormat` | `BayerRG8`（可配） | 用符号名下发的意义是换台相机不用改代码 |
| 4 | `OffsetX/Y` → `Width` / `Height` | 0 / 0 / 全幅 | 像素格式变了以后幅面范围会变，所以放在第 3 步之后；改幅面前必须先把偏移归零 |
| 5 | 读回 `Width` / `Height` | 实际值 | 用相机回读值建缓冲区，不信任配置 |
| 6 | `ExposureTime` / `Gain` / `Gamma` | 8000 us / 0 dB / 不干预 | 全部限幅到 `[dMin, dMax]` 并回读 |
| 7 | `DeviceLinkThroughputLimit` | `nMax` | **大恒 USB3 出厂限速**，不拉满帧率只有理论值的几分之一，这是"帧率上不去"的头号原因 |
| 8 | 数据流句柄 + `GXGetPayLoadSize` | 序号 1 | 大恒的"流"是独立句柄，payload 要从流上查 |
| 9 | `GXSetAcqusitionBufferNumber` | 5 | 越大越抗抖动，但每块都占 `payload` 字节内存 |
| 10 | `PixelColorFilter` | 由相机读出 | Bayer 转换必须传对排列，否则颜色整体错乱 |

## 八、相对培训 demo 的工程化改动

1. **所有 `std::cout` 换成 `RM_LOG_*`**，日志带时间戳、级别、线程、文件行号，并且落盘可追溯。
2. **`GXOpenDeviceByIndex`（legacy）→ `GXOpenDevice` + `GX_OPEN_PARAM`**：现版本 SDK 的标准接口，支持按序列号打开、可指定独占访问模式。
   培训 demo 里那个函数来自 `GxIAPILegacy.h`，能用但不适合做工程基线。
3. **取流逻辑收进 `DahengCamera` 类**：`main()` 只负责参数、主循环与退出码，配置成为 `CameraConfig` 数据而非散落的魔法数字。
4. **`GXQBuf` 归还缓冲收敛到唯一出口**：坏帧、转换失败、状态异常都必须归还，漏还的症状是队列耗尽、`GXDQBuf` 永远超时——这类 bug 最难查，所以不让它有第二条路径。
5. **像素格式按帧头自适应**：Bayer8 / Mono8 / RGB8 / BGR8 都能显示；高位深格式只报一次错，不刷屏。
6. **`ErrorString()` 修正**：培训版用 `new char[size]` 手动管理并可能越界写 `'\0'`，这里改成 `std::vector<char>(size + 1)`。
7. **收尾与异常**：`~DahengCamera()` 保证停流→关设备，`SIGINT/SIGTERM` 走正常收尾，异步日志退出前 `Close()` 保证 flush。

## 九、故障排查

**枚举不到设备 / `GXInitLib` 就失败**

1. 相机是否上电，是否插在 USB3.0 口（蓝色口，USB2 口带不动满帧）；
2. Linux 是否装了 GalaxySDK 的 udev 规则：`/etc/udev/rules.d/99-galaxy-dev.rules`（装 SDK 时会带，缺了就 `sudo cp` 后 `sudo udevadm control --reload`）；
3. `ldd /usr/lib/libgxiapi.so | grep "not found"` 确认没有缺依赖；
4. **WSL 里跑要先做 USB 直通**，否则 `/dev/bus/usb` 根本不存在，SDK 一定在 `GXInitLib` 失败。以下命令已在本项目上实测走通（相机：`2ba2:4d55` MER-230-168U3C，BUSID `2-3`）：

   ```powershell
   # ── Windows 侧（管理员 PowerShell，只需执行一次 bind）──
   usbipd list                              # 找到相机的 BUSID，例如 2-3
   usbipd bind   --busid 2-3                # 一次性：把设备标记为 Shared
   usbipd attach --wsl --busid 2-3          # 每次 WSL 重启后都要重新 attach
   ```
   ```bash
   # ── WSL 侧验证 ──
   lsusb        # 应出现: ID 2ba2:4d55 Daheng Imaging MER-230-168U3C
   ls -l /dev/bus/usb/*/*   # 相机节点权限应为 crw-rw-rw-（GalaxySDK 的 99-galaxy-dev.rules 生效）
   ```

   三个坑：
   - `bind` / `attach` **都必须管理员权限**，普通终端会报 `Access denied`；
   - **attach 前 WSL 发行版必须处于运行状态**，否则报 `There is no WSL 2 distribution running`——先开一个 WSL 终端（跑个 `sleep`）再 attach；
   - **WSL 一重启（含 `wsl --shutdown`）直通就失效**，重新 `usbipd attach` 即可，`bind` 不用重做。

   不想折腾直通时，直接在接了相机的原生 Linux 机器上编译运行。

**`GXDQBuf` 一直超时**

曝光是否过长（超时阈值默认 1000 ms，曝光 8 s 时必然超时，`--timeout` 改大）；
`TriggerMode` 是否被相机掉电后恢复成了 `On`；是否有别的进程占着相机（本项目用独占模式打开，会明确报错而不是静默抢帧）。

**帧率低**

看日志里 `DeviceLinkThroughputLimit 当前 … 范围 [min, max]`：默认已拉满。仍未达标则看 USB 口（USB2 口限速）、
`GXSetAcqusitionBufferNumber` 是否太小、单帧转换是否成为瓶颈（`--no-display` 对比一下）。

**画面颜色不对（红蓝互换 / 偏绿）**

`PixelColorFilter` 与实际 Bayer 排列不匹配：看日志里 `PixelColorFilter = RG -> DX_PIXEL_COLOR_FILTER(1)` 一行，
再对照 `--pixel-format` 设置的值。注意 `BayerRG8` 的转换结果在 OpenCV 里应当传 `DX_ORDER_BGR`（本项目已处理）。

**日志里没有输出**

`rm_log` 的宏直接解引用 logger，`Init` 之前调用会段错误——本项目的 `INIT_LOG` 是 `main()` 的第一件事。
另外日志文件级别默认 `info`，DEBUG 的每帧信息只在控制台可见，需要落盘就用 `--file-level debug`。

## 十、验证状态

已在真实相机（大恒 **MER-230-168U3C**，SN `NE0190010001`，USB3.0 直通进 WSL Ubuntu 22.04）上完整跑通：

| 项 | 状态 |
|---|---|
| Ubuntu 22.04 + g++ 11.4 + CMake + OpenCV 4.5.5 下编译链接 | ✅ 0 error（仅大恒头文件自带的 1 条 `typedef` 警告） |
| 链接真实 `libgxiapi.so`（`GXOpenDevice` / `DxRaw8toRGB24Ex` 符号解析） | ✅ 通过 |
| `rm_log` 控制台彩色输出 + 滚动日志文件落盘 + `--help` / 参数解析 / 错误路径 | ✅ 通过 |
| `--list` 枚举设备 | ✅ 识别出 `[1] U3V(USB3.0) | 厂商: Daheng Imaging | 型号: MER-230-168U3C | 序列号: NE0190010001` |
| 配置下发（复现第 7 节全部 10 步） | ✅ `PixelFormat=BayerRG8`（可选值 `BayerRG8 / BayerRG10`）· 分辨率 `1920x1200` · `PixelColorFilter=RG → BAYERRG(1)` · payload `2304000` 字节/帧 · `DeviceLinkThroughputLimit` 由 `400000000` 拉到 `nMax=400000000`（该型号出厂即为最大值）· 取流缓冲 `5` |
| 真实取流 | ✅ 连续 100 帧 / 50 帧两次运行，**坏帧 0、超时 0**；无窗口模式 **57.45 fps @1920×1200**，带窗口显示 **33.28 fps**（差值即 `imshow` 开销） |
| 窗口显示图像（`cv::imshow`） | ✅ WSLg 下 `DISPLAY=:0` `WAYLAND_DISPLAY=wayland-0`，30 帧显示运行无异常、无降级告警 |
| Bayer → BGR 转换与存图 | ✅ 存出 1920×1200 PNG（1.48 MB），画面内容与颜色经目视确认正确 |
| 完整 DEBUG 日志 | ✅ 见 `docs/sample_run.log`（143 行，含全部配置节点与每秒状态） |

> 复现用的完整命令行：`./build/daheng_camera_demo --serial NE0190010001 --frames 100 --no-display --file-level debug`

## 十一、License

MIT，见 [LICENSE](LICENSE)。`include/sdk/*` 与 `util/spdlog/*` 分别为大恒官方 SDK 头文件与 spdlog（MIT）源码，版权归各自作者。
