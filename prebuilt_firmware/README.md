# Prebuilt Merged Firmware (`0x0`) / 预编译一体化固件

Merged single-file factory binaries (bootloader + partition table + `ota_data`/`phy_init` + application + AI model partitions) can be downloaded from **[GitHub Releases](https://github.com/HanerQAQ/ESP32-P4C5-CAM/releases)** or generated locally via ESP-IDF v6.1 (`idf.py merge-bin`). All merged images flash cleanly at offset **`0x0`**.

本目录用于存放通过 ESP-IDF v6.1 `idf.py merge-bin` 生成的单文件完整合并固件（已包含引导程序、分区表、AI 模型及应用程序），烧录起始地址统一为 **`0x0`**。可前往 **[GitHub Releases](https://github.com/HanerQAQ/ESP32-P4C5-CAM/releases)** 下载官方发布二进制包，或在本地执行一键批量构建生成。

---

## Binary Matrix / 固件清单

| Merged Binary (`0x0`) | Function / 功能说明 | Default Access / 默认热点与 I²C |
| :--- | :--- | :--- |
| `Image_transmission_function_STA.bin` | AP/STA Dual-Mode Streaming (Factory Default) | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x52` |
| `Image_transmission_function_AP.bin` | AP-Only 5 GHz MJPEG Streaming | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C Test `0x54` |
| `hand_gesture_recognition.bin` | 10-Class Hand Gesture Recognition | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `cat_detect.bin` | Cat Detection | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `dog_detect.bin` | Dog Detection | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `pedestrian_detect.bin` | Pedestrian Detection | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `coco_detect.bin` | YOLO11n 80-Class Object Detection | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `human_face_detect.bin` | Human Face Detection | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `coco_pose.bin` | 17-Keypoint Human Pose Estimation | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `apriltag_recognition.bin` | AprilTag (`tag36h11`) 6-DoF Pose | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `qrcode_recognition.bin` | QR Code Decoding | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |
| `color_detect.bin` | Color Blob & Line Tracking | SoftAP `ESP32-P4C5-CAM-XXXXXX` / I²C `0x54` |

To generate all 12 merged binaries locally with **ESP-IDF v6.1**:

```bash
cd firmware
python build_and_verify_all.py --clean
```
