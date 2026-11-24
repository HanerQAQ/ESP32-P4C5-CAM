# MJPEG Streaming AP Mode (Image_transmission_function_AP)

**English** | [简体中文](#简体中文)

Standalone SoftAP MJPEG streaming firmware for ESP32-P4C5-CAM (**2.4 GHz Channel 1 by default** for broad compatibility; switchable to **5 GHz Wi-Fi 6** via `idf.py menuconfig` -> `Example Configuration` -> `Wi-Fi AP band`). Streams live video over Web port `81` (`/stream`), provides an interactive browser UI on port `80`, runs on-demand AprilTag (`tag36h11`) detection infrastructure on CPU1, and verifies external I²C / BOOT functionality.

- **Testing Guide**: [TEST_GUIDE_en.md](TEST_GUIDE_en.md) | [中文说明](TEST_GUIDE_zh.md)

## Build & Flash

```bash
cd firmware/Image_transmission_function_AP
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # Replace COMx with your actual serial port
```

## Default Network & Web Service

- **SoftAP SSID**: `ESP32-P4C5-CAM-XXXXXX` (Default: 2.4 GHz Channel 1, Open; optional 5 GHz Channels 36/40/44/48 via `menuconfig`)
- **Web UI**: `http://192.168.5.1/` (port 80)
- **MJPEG Stream**: `http://192.168.5.1:81/stream` (port 81)
- **Detections API**: `http://192.168.5.1/api/detections`
- **Hardware Test I²C**: Address `0x54` (SCL = GPIO 47, SDA = GPIO 46)

---

## 简体中文

# 图像回传 AP 模式工程 (Image_transmission_function_AP)

测试流程、接线、通过标准及记录表见 [测试说明 (中文)](TEST_GUIDE_zh.md) | [English Guide](TEST_GUIDE_en.md)。

本工程基于 **ESP32-P4** 采集 RGB565 视频流，内置 `who_apriltag` 同源的 OpenMV AprilTag 引擎，可通过网页实时查看 MJPEG 视频与识别测试信息（无板载 LCD）。

模块分配两组独立 I²C：摄像头 SCCB 位于 `SCL=GPIO29`、`SDA=GPIO28`（`100 kHz`）；外部 I²C1 从机位于 `SCL=GPIO47`、`SDA=GPIO46`（7 位地址 `0x54`）。`BOOT` 按键使用 `GPIO35`（低电平有效，约 30 ms 消抖）。

`/api/tests` 接口提供三项硬件测试状态（本轮通电周期内保持，重启后清零）：

- `camera`：摄像头成功输出或识别标签后置 `1`（用于硬件链路自检）；
- `boot`：运行期间检测到底板 `GPIO35` 按键按下后置 `1`；
- `i2c`：外部 I²C 主机单次原始写事务发送 14 字节 ASCII `ESP32P4C5-MINI`（不含寄存器前缀、NUL 或换行）后置 `1`。

I²C 主机需与模组共地并使用 3.3 V 逻辑电平（建议 100 kHz）。Linux 主机测试示例（`BUS` 替换为实际总线编号）：

```bash
i2ctransfer -y BUS w14@0x54 0x45 0x53 0x50 0x33 0x32 0x50 0x34 0x43 0x35 0x2d 0x4d 0x49 0x4e 0x49
```

`GET /api/tests` 返回 `camera`、`boot`、`i2c` 及 `all_passed` 字段（`0`=等待、`1`=通过、`2`=未配置、`3`=初始化失败）。引脚可在 `idf.py menuconfig -> Hardware test pins` 调整。

## 构建与运行

本工程使用 **ESP-IDF v6.1**，共享组件位于 `../components`：

```bash
source /path/to/esp-idf/export.sh
cd firmware/Image_transmission_function_AP
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # 将 COMx 替换为实际串口号
```

若使用 Windows 烧录，建议在相同 ESP-IDF v6.1 环境下直接执行 `idf.py -p COMx flash`，或使用构建产物中的 `flasher_args.json` 烧录。

SDIO 无线协处理器复位配置为 `GPIO50`、高电平有效、复位延时 `700 ms`，每次上电均复位 ESP32-C5。

热点默认配置（`sdkconfig.defaults`）为 **2.4 GHz 信道 1（开放热点，兼容全部终端）**；如需使用 **5 GHz + Wi-Fi 6（802.11ax）**，可在 `idf.py menuconfig -> Example Configuration -> Wi-Fi AP band` 中切换为 5 GHz 模式（20 MHz 带宽，在 36/40/44/48 信道中自动选择，扫描失败时回退到信道 36）。

烧录后连接串口日志显示的 Wi-Fi AP（`ESP32-P4C5-CAM-XXXXXX`），打开浏览器访问 `http://192.168.5.1`。视频流使用端口 `81`，状态接口为 `GET /api/detections`。

传感器原生采集尺寸为 `800×600`，上电默认以 **`640×480`** 居中窗口输出以兼顾高帧率流畅度（约 30 fps），可在网页 `Settings` 中手动切换为完整 **`800×600`** 分辨率。网页保留实时预览、JPEG 下载、原始 RGB565 图像下载与质量设置。

CPU0 视频任务始终以优先级 5 负责采集、编码和推流，视频任务栈为 8 KB；建议测试时只开启单个浏览器视频页签以获得最佳帧率。

灰度缓冲区固定分配在 PSRAM，避免占用 ESP-Hosted SDIO 发送所需的内部 DMA 内存。启动日志打印内部 DMA 可用总量和最大连续块。若仍出现 `AP TX: mempool_alloc failed`，需结合这些数值和视频发送 FPS 进一步检查内存压力及发送流量。

CPU1 识别任务以 `tskIDLE_PRIORITY` 运行，AprilTag 的 `nthreads=1`，通过抢占式同优先级时间片与 IDLE1 分享 CPU，并在每次处理后延时 1 tick。没有图像时阻塞等待通知。保持 5 秒任务看门狗及两个核心的空闲任务监控。识别开始、完成及耗时日志为 DEBUG 级别。检测框来自最近完成的识别帧，可能滞后于实时视频。
