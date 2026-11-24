# MJPEG Streaming AP Mode (Image_transmission_function_AP)

**English** | [简体中文](#简体中文)

Standalone SoftAP Wi-Fi 6 (5 GHz) MJPEG streaming firmware for ESP32P4C5-Cam. Streams live video over Web port `81` (`/stream`), provides an interactive browser UI on port `80`, runs on-demand AprilTag (`tag36h11`) detection on CPU1, and verifies external I²C / BOOT functionality.

- **Testing Guide**: [TEST_GUIDE_en.md](TEST_GUIDE_en.md) | [中文说明](TEST_GUIDE_zh.md)

## Build & Flash

```bash
cd firmware/Image_transmission_function_AP
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # Replace COMx with your actual serial port
```

## Default Network & Web Service

- **SoftAP SSID**: `ESP32-P4C5-CAM-XXXXXX` (5 GHz, Channel 36/40/44/48, Open)
- **Web UI**: `http://192.168.5.1/` (port 80)
- **MJPEG Stream**: `http://192.168.5.1:81/stream` (port 81)
- **Detections API**: `http://192.168.5.1/api/detections`
- **Hardware Test I²C**: Address `0x54` (SCL = GPIO 47, SDA = GPIO 46)

---

## 简体中文

# 图像回传 AP 模式工程 (Image_transmission_function_AP)

测试流程、接线、通过标准及记录表见 [测试说明 (中文)](TEST_GUIDE_zh.md) | [English Guide](TEST_GUIDE_en.md)。

本工程在 ESP32-P4C5-CAM ESP32-P4 上采集 RGB565 视频，使用 `who_apriltag` 同源的 OpenMV AprilTag 实现识别，
并通过网页显示 MJPEG 视频、标签四边形、ID、中心点、位移和旋转信息。不初始化或使用 LCD。

按参考项目分配两组独立 I2C：摄像头 SCL GPIO29、SDA GPIO28（100 kHz）；外部设备 I2C1 从机 SCL GPIO47、SDA GPIO46（7 位地址 0x54）。BOOT 使用 GPIO35（低电平有效，约 30 ms 消抖），请在设备启动后按下。

网页提供三项测试，通过状态在本次运行中保持，重启设备清除：

- 摄像头：打开网页视频，将 tag36h11 对准镜头，按 BOOT 切换到 AprilTag 实时识别模式，识别到任意标签即通过；此条件是识别能力测试，不提供定量清晰度评分。
- BOOT：运行后按下 GPIO35 对应的 BOOT 键即通过。
- 外部 I2C：主机一次原始写事务发送 `ESP32P4C5-MINI`，严格匹配 14 个 ASCII 字节，无寄存器前缀、NUL 或换行；其他内容不通过。

I2C 主机与设备需共地，使用 3.3 V 电平和适当的总线上拉，建议以 100 kHz 测试。Linux 主机示例（将 `BUS` 换成总线编号）：

```bash
i2ctransfer -y BUS w14@0x54 0x45 0x53 0x50 0x33 0x32 0x50 0x34 0x43 0x35 0x2d 0x4d 0x49 0x4e 0x49
```

`GET /api/tests` 返回 camera、boot、i2c 状态及 all_passed。状态 0=等待、1=通过、2=未配置、3=初始化失败。
引脚可在 `menuconfig -> Hardware test pins` 配置。摄像头初始化失败时仍尝试启动测试网页，方便测试另外两项。

## 构建与运行

本交付副本使用 **ESP-IDF v6.1**，共享的 `example_video_common` 位于
`../components/example_video_common`：

```bash
source /path/to/firmware/export.sh
cd firmware/Image_transmission_function_AP
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # 将 COMx 替换为设备在设备管理器中的实际串口号
```

在 WSL 中构建、在 Windows 中使用 COM 端口烧录时，先在 WSL 执行
`idf.py build`，再使用项目构建目录中的 `flasher_args.json` 或
`idf.py -p COMx flash`（Windows 端 IDF 环境）烧录。

SDIO 从机复位与 `simple_video_server_ap` 的 ESP32-P4C5-CAM 配置一致：GPIO50、高电平有效、复位延时 700 ms，每次主机启动均复位从机。

热点默认使用 **5 GHz + Wi-Fi 6（802.11ax）**，20 MHz 带宽，在 36/40/44/48 信道中自动选择，扫描失败时回退到信道 36。保留 a/n/ac 协议兼容；实际是否使用 Wi-Fi 6 取决于客户端能力。连接设备需支持 5 GHz，不再提供 2.4 GHz 热点。

烧录后连接串口日志显示的 Wi-Fi AP（默认配置见 `menuconfig -> Example Configuration`），然后访问
`http://192.168.5.1`。视频流使用 81 端口，识别结果接口为 `GET /api/detections`。
Factory 例程保留原网页与测试逻辑；本项目使用 `ESP32-P4C5-CAM-` 开头的
5 GHz 开放热点，与非 Factory 例程的 `ESP32-P4C5-CAM-` 热点不同。两个方向的
例程不要同时上电测试。

当前识别标签族为 `tag36h11`。上电默认图传模式，不运行检测。短按 BOOT（GPIO35）在图传与实时识别之间切换；长按不重复触发。关闭视频流时不处理帧。切回图传时，正在执行的检测允许完成，旧模式结果不会显示。

网页复用 `simple_video_server_ap/frontend/gzipped` 的原始 JS/CSS、图标与加载图，保留预览、JPEG 下载、原始图像下载、格式/质量设置和重连功能。当前 `frontend/index.html` 在原网页入口上增加模式提示和 SVG 检测框；不修改参考项目。截图和原始图像下载不包含网页叠加框。

视频、截图及原始图像均保留传感器完整尺寸（当前 800×600），不裁剪。仅在输入带行填充时压紧行数据。AprilTag 输入沿宽高各降采样 4 倍：800×600 → 200×150，对每个 4×4 RGB565 像素块的灰度取平均。灰度缓冲位于 PSRAM。`quad_decimate=1`，不再二次降采样；小标签需靠近镜头。检测坐标映射回完整画面，网页显示轮廓框及 ID，空检测、模式切换、连接失败或结果超过 2 秒时清除。

CPU0 视频任务始终以优先级 5 采集、编码和发送；CPU1 独立 AprilTag 任务识别。识别空闲时，视频任务将当前帧降采样到独立 PSRAM 灰度缓冲并通知识别任务，立即继续图传，不等待识别完成。识别忙时跳过帧，不排队；完成后提交下一张新帧。识别任务不持有相机缓冲区，避免相机被阻塞或图像被覆盖。视频任务栈为 8 KB，识别任务栈为 32 KB。实际帧率与双核内存竞争影响仍需上板测量。

相机内参沿用原 `who_apriltag` 的 320x240 参数并随识别分辨率缩放；
如需精确姿态，请在 `main/detection_service.c` 中替换标定后的 `fx/fy/cx/cy`。

灰度缓冲区固定分配在 PSRAM，避免占用 ESP-Hosted SDIO 发送所需的内部 DMA 内存。启动日志打印内部 DMA 可用总量和最大连续块。若仍出现 `AP TX: mempool_alloc failed`，需结合这些数值和视频发送 FPS 进一步检查内存压力及发送流量。

CPU1 识别任务以 `tskIDLE_PRIORITY` 运行，AprilTag 的 `nthreads=1`，通过抢占式同优先级时间片与 IDLE1 分享 CPU，并在每次处理后延时 1 tick。没有图像时阻塞等待通知。保持 5 秒任务看门狗及两个核心的空闲任务监控。识别开始、完成及耗时日志为 DEBUG 级别。检测框来自最近完成的识别帧，可能滞后于实时视频。
