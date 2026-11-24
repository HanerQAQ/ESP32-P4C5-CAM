# Pedestrian Detection (pedestrian_detect)

**English** | [简体中文](#简体中文)

Real-time edge AI pedestrian detection running on ESP32-P4 with live Web MJPEG streaming and I²C slave output (0x54).

- **Matching Host Example**: `examples/<platform>/04_pedestrian/`
- **I²C Slave Interface**: Address `0x54`, `400 kHz` (SDA = GPIO 46, SCL = GPIO 47)

## Build & Flash

```bash
cd pedestrian_detect
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

# 行人检测 · Web 回传

本目录为 `pedestrian_detect` 离线视觉例程（离线行人目标检测），内置 I²C 从机（`0x54`）协议输出，并支持 Wi-Fi MJPEG 实时图传与检测结果 Web 叠加预览。

## 构建

```bash
source /path/to/esp-idf/export.sh     # 激活 ESP-IDF v6.1 环境
cd pedestrian_detect
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # 将 COMx 替换为实际串口号
```

共享组件位于 `../components`，需要和本目录一起保留。

## 连接与访问

| 项目 | 默认值 |
| --- | --- |
| SoftAP | `ESP32-P4C5-CAM-` + AP MAC 后三字节 |
| 密码 | 空（开放热点） |
| 频段 / 信道 | 5 GHz / 36 |
| 网页 | `http://192.168.5.1/` |
| MJPEG | `http://192.168.5.1:81/stream` |
| 结果 JSON | `http://192.168.5.1/api/detections` |

连接热点后打开网页。页面显示：行人框与置信度。

默认 AP 使用 5 GHz；若客户端不支持，执行 `idf.py menuconfig`，在
`ESP32-P4C5-CAM web backhaul` 中关闭 `Use 5 GHz only` 并把信道改为 6。
建议只打开一个视频页面。

## 结果说明

Web 结果类型为 `pedestrian`，对象数组字段说明见上层 `../README.md`。
视频画面与检测框统一使用 320×240 推理帧坐标系；检测结果更新时由推理回调实时写入。
只有存在视频客户端时才会进行 JPEG 编码。

开放热点没有认证，仅适合本机/可信局域网演示。详细实现见
`../components/who_web/README.md` 和上层 `../README.md`。
