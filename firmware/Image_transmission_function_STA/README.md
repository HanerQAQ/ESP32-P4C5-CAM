# MJPEG Streaming STA+AP Dual-Mode (Image_transmission_function_STA)

**English** | [简体中文](#简体中文)

Production-grade factory firmware for ESP32P4C5-Cam. Features **dual-mode Wi-Fi (STA regular mode + SoftAP fallback/on-demand config)**, hardware-accelerated **MJPEG streaming over Wi-Fi 6 (802.11ax)**, full REST API backwards compatibility, 32-byte fragmented **ESP32-P4C5-CAM I²C slave protocol (`0x52`)**, physical BOOT button pairing, and UART CLI console.

## Build & Flash

```bash
cd firmware/Image_transmission_function_STA
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # Replace COMx with your actual serial port
```

## Default Network & Web Service

- **Fallback SoftAP**: `ESP32-P4C5-CAM-XXXXXX` (Channel 36/6, Open, gateway `192.168.5.1`)
- **Web UI & Provisioning**: `http://192.168.5.1/` (switches to camera stream once connected to router)
- **Direct Wi-Fi Setup**: `http://192.168.5.1/wifi`
- **MJPEG Stream**: Port 81 (`http://<IP>:81/stream`) or redirected from port 80 (`/stream`)
- **I²C Slave**: Address `0x52` (SCL = GPIO 47, SDA = GPIO 46, 400 kHz)

---

## 简体中文

# HanerQAQ ESP32-P4 + C5 图像回传 STA+AP 双模版本 (Image_transmission_function_STA)

本例程基于 **ESP-IDF 6.1** 与 **ESP32-P4 + ESP32-C5 (SDIO ESP-Hosted)** 协处理器架构开发。深度参考并对齐了 **ESP32-S3** `Camera_Display_STA/esp32Board_wifi` 的全部功能与协议规范，实现了具备 **Web 配网（无强制门户）**、**STA 常态 + SoftAP 按需切换**、**32 字节 ESP32-P4C5-CAM I2C 从机通信**、**BOOT 物理按键交互**、**UART CLI 终端控制** 与 **S3 完全兼容 REST API** 的高性能图像回传系统。

---

## 一、 系统架构与优化特性（对比 S3 增强对齐）

1. **STA 常态 + SoftAP 按需配网（对齐 capture_stream_test 参考固件）**：
   - STA 连接参数与参考固件 `example_connect()` 一致：全信道扫描、按信号强度选路、最多 1 次快速重连（每次连接另有 8 秒看门狗）。
   - **双频 STA**：支持 2.4GHz 与 5GHz；配网页扫描列表为每个网络显示 `2.4G` / `5G` 标签。点击某个扫描条目后提交，会按该条目的 BSSID/信道连接，可在同一 SSID 双频时强制选择 5GHz 或 2.4GHz；已保存网络卡片在选中同一 SSID 的 5G 条目时也会用已保存密码强制 5G。
   - **跨网络切换提示**：从 STA 页面切换到另一个 Wi-Fi 时，页面会立即（约 0.5 秒）提示目标网络；不使用 mDNS，切换后请通过串口每 30 秒打印的 STA IP 或路由器客户端列表找到新 IP。跨网络配网推荐使用 5G AP 配网模式，AP 会跟随所选 STA 频段。
   - 无凭据或 STA 连续失败时自动开启 SoftAP 配网热点：`ESP32-P4C5-CAM-XXXXXX`（`XXXXXX` 为 AP MAC 后 3 字节大写十六进制，开放无密码，网关 `192.168.5.1`）。
   - **长按 BOOT ≥1 秒**会断开当前 STA 连接并进入 **5G AP 配网模式**（STA 保持断开、不自动重连，不删除 NVS 凭据）；配网页会显示“已保存网络”，可一键直接连接，无需重新输入账号密码。
   - 配网页扫描仍包含 2.4G/5G 网络，用户可选择任一频段的 BSSID/信道。提交后 **AP 自动跟随目标 STA 频段/信道**并与 STA 同时工作；切换提示弹窗会一直等待 STA 连接，连接成功后立即显示 **STA IP** 弹窗并保持 30 秒，30 秒后自动跳转到 `http://<STA IP>/`。AP 在 STA 获取 IP 后保持约 15 秒再关闭。
   - **不使用 mDNS/二维码/屏幕**：设备发现和访问仅使用 IP；AP 配网使用 `http://192.168.5.1/wifi`，STA 模式使用串口输出的 STA IP。设备 hostname `P4C5CAM` 仍通过 DHCP 上报，便于在路由器客户端列表中识别。
   - STA 掉线先快速重试 1 次（间隔 1 秒，单次最多 8 秒），仍失败则回退 5G AP 配网模式；AP 配网期间 STA 初始空闲，提交后 AP 跟随目标频段。DFS 5G 信道无法承载 SoftAP，遇 DFS 目标时自动回退为“先关 AP 再连 STA”（IP 看串口）。

2. **Captive Portal 已关闭**：
   - 不启动 DNS 劫持服务器，不设置 DHCP option 114，也不拦截/重定向系统门户探测请求。受 ESP-IDF 6.1 DHCP 服务器默认行为影响，客户端可能仍看到 AP IP 作为 DNS；但设备不运行 DNS 服务，也不做任何门户导向。
   - 连接设备 AP 后，请直接在浏览器访问 `http://192.168.5.1/wifi`。
   - 电脑若同时连接有线、其他 Wi-Fi 或代理，Windows 的“Open browser and connect”可能仍打开微软探测页；请手动使用上述 IP，或临时断开其他网络。
   - AP 仅保留普通 DHCP（IP/网关）和本地 HTTP 配网页，不再做自动门户导向。
   - 前端配网页面集成中英文详细断连原因映射（如：“密码错误”、“未找到指定 AP”等）。

3. **智能根路由（Smart Root Routing）**：
   - 未连网状态：访问 `http://192.168.5.1/` 自动展示 Wi-Fi 配网页面。
   - 已连网状态：访问 `http://<IP>/` 自动切换为 Vue 摄像头图传控制主界面。
   - 任何状态下，访问 `/vue` 或 `/cam` 均可强制直达图传主页；访问 `/wifi` 随时打开配网后台。

4. **80 端口与 81 端口流无缝兼容**：
   - 80 端口支持 `/stream` 访问：自动 302/307 重定向至 `http://<IP>:81/stream`，兼容各类仅识别 80 端口 `/stream` 的上位机或播放器。
   - 81 端口运行高速独立硬件 MJPEG 视频流（`multipart/x-mixed-replace`）。

5. **S3 完全兼容 REST 与 控制 API**：
   - 支持传统的 `/control?var=hmirror&val=1`、`/control?var=vflip&val=1`、`/control?var=quality&val=30`、`/control?var=framesize&val=6`（VGA）/`val=7`（SVGA）。
   - 支持 S3 格式的 `/status` 查询接口（返回 JSON 格式的分辨率、翻转、质量、FPS 等参数）。
   - 支持单张快照抓图：`/capture`、`/api/frame.jpg` 与 `/api/capture_image`（支持缺省 source 参数直接抓取）。
   - 支持现代配置接口：`POST /api/set_camera_config` 与 `POST /api/resolution`。

6. **ESP32-P4C5-CAM 32 字节分包 I2C 从机协议（地址 0x52）**：
   - 引脚：**SCL = GPIO 47**，**SDA = GPIO 46**。
   - 完全对齐 S3 `my_user_iic.c` 与ESP32-P4C5-CAM上位机主控（STM32 / Arduino / MicroPython）通信协议。
   - 支持帧头 `0xAA 0x55` 32 字节分包接收与回复校验（XOR 校验和）。
   - 支持 JSON 指令：
     - 查询状态：`{"command":"get_info"}`，快速返回 IP、分辨率、翻转及 FPS 信息。
     - 图像调节：`{"command":"vision_setting","params":{"hmirror":1,"vflip":1,"quality":35}}`。
     - 软复位：`{"command":"module_reset"}`。
   - 支持传统 2 字节寄存器直接写入：
     - `[0x01, val]`：水平镜像翻转（hmirror）。
     - `[0x02, val]`：垂直镜像翻转（vflip）。
     - `[0x04, 0x01]`：系统重启。

7. **BOOT 物理按键交互（GPIO 35）**：
   - **短按**：仅在串口控制台打印系统状态横幅，不影响当前网络。
   - **长按 ≥1 秒**：断开当前 STA 连接并进入纯 AP 配网模式；NVS 中的路由器凭据保留，配网页“已保存网络”卡片可一键重连。
   - 配网提交后 AP 跟随用户选择的 2.4G/5G STA 频段；切换提示一直等待，连接成功时立即弹窗显示 STA IP，30 秒后自动跳转到 `http://<STA IP>/`；AP 约 15 秒后关闭。串口每 30 秒打印一次 STA IP。AP 模式每 30 秒打印 AP 名称、AP IP 和 5G 信息。
   - 串口命令 `boot` 可查看 BOOT 输入电平与按下次数；`provision_ap` 可远程触发配网模式（断开 STA）。
   - 需要清除已保存凭据时，使用串口命令 `reset_wifi` 或配网页面中的 “Clear Saved” 按钮。

8. **UART0 串口交互 CLI 终端**：
   - 波特率 115200，提供交互式命令行支持：
     - `sta_ip`：查看路由器 Station 分配的 IP。
     - `ap_ip`：查看板载 SoftAP 的 IP。
     - `status`：查看当前硬件及网络状态 JSON。
     - `reset_wifi`：清除配网信息并自动重启。
     - `reboot`：重启系统。
     - `help`：显示命令帮助。

---

## 二、 HTTP 接口清单

| 路径 | 方法 | 端口 | 描述 |
| :--- | :---: | :---: | :--- |
| `/` | GET | 80 | 智能根路由（已连网则展示图传主页，未连网展示配网页） |
| `/wifi` | GET | 80 | 路由器 Wi-Fi 扫描与配网网页界面 |
| `/vue` 或 `/cam` | GET | 80 | 摄像头 Vue 图传主界面直达 |
| `/scan` | GET | 80 | 扫描周边 Wi-Fi 列表（JSON，含 SSID、RSSI、信道、`2.4G`/`5G` 频段与 BSSID） |
| `/submit` 或 `/connect` | POST | 80 | 提交 Wi-Fi 凭据并异步连接（`{"ssid":"xxx","password":"xxx"}`）；可选 `bssid`+`channel` 强制频段；`{"ssid":"xxx","use_saved":true}` 使用已保存密码一键重连 |
| `/api/wifi/saved` | GET | 80 | 查询已保存的路由器 SSID（仅返回 SSID，不返回密码） |
| `/connect/status` | GET | 80 | 查询当前网络连接状态（含 `ap_active`、`ap_ip`、`ap_ssid`、STA IP、`sta_band`/`sta_channel` 与错误描述 `message`） |
| `/api/wifi/reset` | GET/POST | 80 | 清除 NVS 保存的 Wi-Fi 凭据并重置为待配网状态 |
| `/capture` 或 `/api/frame.jpg` | GET | 80 | 抓取当前单张 JPEG 高清图像（兼容 S3） |
| `/api/capture_image` | GET | 80 | 采集当前单张 JPEG 高清图像（支持 `?source=0`，缺省默认为 0） |
| `/control` | GET | 80 | 动态调参（例如 `/control?var=hmirror&val=1`、`framesize`、`quality`） |
| `/status` | GET | 80 | 获取当前摄像头运行状态与传感器参数（JSON 格式） |
| `/api/get_camera_info` | GET | 80 | 获取摄像头分辨率、支持格式及流地址 |
| `/api/set_camera_config` | POST | 80 | 现代 REST 参数配置（分辨率切换与 JPEG 质量设置） |
| `/api/resolution` | POST | 80 | 分辨率切换接口（兼容 S3） |
| `/api/detections` | GET | 80 | 获取当前图像状态与实时视频 FPS |
| `/stream` | GET | 80 | 自动 302/307 重定向至 `http://<IP>:81/stream` |
| `/stream` | GET | 81 | 硬件 MJPEG 实时视频流（`multipart/x-mixed-replace`） |

---

## 三、 硬件引脚分配

| 功能引脚 | ESP32-P4 GPIO | 说明 |
| :--- | :---: | :--- |
| I2C SCL (从机) | GPIO 47 | 接上位机主控 I2C SCL，板内具备弱上拉 |
| I2C SDA (从机) | GPIO 46 | 接上位机主控 I2C SDA，板内具备弱上拉 |
| BOOT 按键 | GPIO 35 | 短按打印状态；长按 ≥1 秒断开 STA 并进入 AP 配网 |
| UART0 TX / RX | 默认控制台 | 115200 8N1，支持命令交互终端 |
| GC2145 MIPI-CSI | 专用 MIPI 接口 | 硬件双缓冲图像捕获与硬件 JPEG 压缩 |
| ESP32-C5 SDIO | 专用 SDIO 接口 | ESP-Hosted Wi-Fi 6 通信链路 |

---

## 四、 固件编译与单固件导出

### 1. 源码编译 (ESP-IDF 6.1)
```bash
# 切换到工程目录
cd firmware/Image_transmission_function_STA

# 设置目标芯片并构建
idf.py set-target esp32p4
idf.py build

# 烧录与串口监视（将 COMx 替换为设备在设备管理器中的实际串口号）
idf.py -p COMx flash monitor
```

### 2. 合并单固件 (0x0 偏移烧录)
编译完成后，可使用 `merge-bin` 生成全量烧录单固件：
```bash
idf.py merge-bin -o Image_transmission_function_STA.bin
```
生成的 `Image_transmission_function_STA.bin` 支持直接以 `0x0` 偏移地址烧录到 Flash 中。
