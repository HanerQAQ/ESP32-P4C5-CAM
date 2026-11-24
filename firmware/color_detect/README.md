# Color Detection & Line Tracking (color_detect)

**English** | [简体中文](#简体中文)

Offline multi-color blob detection and line tracking firmware for ESP32-P4C5-CAM. Features on-device HSV threshold segmentation, live Web MJPEG streaming, unified I²C slave output (`0x54`), and real-time threshold calibration over USB serial.

- **Serial Protocol Specification**: [../../docs/uart_protocol_en.md](../../docs/uart_protocol_en.md) | [中文](../../docs/uart_protocol_zh.md)
- **Desktop Calibration GUI (ColorTool)**: [../../tools/color_tool/README.md](../../tools/color_tool/README.md)

## Build & Flash

```bash
cd color_detect
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # Replace COMx with your actual serial port
```

## Wi-Fi & Web Access

| Parameter | Default Value |
| :--- | :--- |
| **SoftAP SSID** | `ESP32-P4C5-CAM-` + last 3 MAC bytes |
| **Password** | Open (none) |
| **Band / Channel** | 5 GHz / Channel 36 |
| **Web Interface** | `http://192.168.5.1/` |
| **MJPEG Stream** | `http://192.168.5.1:81/stream` |
| **Detections API** | `http://192.168.5.1/api/detections` |

---

## 简体中文

本目录为 `color_detect` 颜色与巡线视觉例程，支持 I²C 从机（`0x54`）输出、Wi-Fi MJPEG 视频与检测结果 Web 实时回传，并可通过底板 Type-C 串口配合 `ColorTool` 实时标定 HSV 阈值。

- **串口通信协议规范**：[../../docs/uart_protocol_zh.md](../../docs/uart_protocol_zh.md)
- **配套桌面端调参工具**：[../../tools/color_tool/README.md](../../tools/color_tool/README.md)

### 构建与烧录

```bash
cd color_detect
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # 将 COMx 替换为设备实际串口号
```

### 连接与访问

| 项目 | 默认值 |
| :--- | :--- |
| **SoftAP** | `ESP32-P4C5-CAM-` + AP MAC 后三字节 |
| **密码** | 空（开放热点） |
| **频段 / 信道** | 5 GHz / 36 |
| **网页** | `http://192.168.5.1/` |
| **MJPEG** | `http://192.168.5.1:81/stream` |
| **结果 JSON** | `http://192.168.5.1/api/detections` |

连接热点后打开网页，页面显示颜色块框或巡线拟合线。
若客户端不支持 5 GHz，执行 `idf.py menuconfig`，在 `ESP32-P4C5-CAM web backhaul` 中关闭 `Use 5 GHz only` 并把信道改为 6。
详细实现见 `../components/who_web/README.md`。
