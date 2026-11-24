# ESP32-P4C5-CAM ESP-IDF Examples (v6.1)

**English** | [简体中文](#简体中文)

This directory contains all 12 **ESP-IDF v6.1** module-side source projects and the shared `components/` library for **ESP32-P4C5-CAM**.

## Directory Layout

- `components/`: Shared drivers and middleware (`bsp`, `who_web`, `who_communicate`, `coco_pose`, `color_detect`, etc.). **Keep `components/` alongside the project folders**, as each `CMakeLists.txt` references `../components/...`.
- Streaming examples:
  - `Image_transmission_function_AP/`: SoftAP-only MJPEG streaming
  - `Image_transmission_function_STA/`: AP/STA dual-mode streaming (factory firmware source)
- Offline AI vision examples (I²C slave `0x54` + Web MJPEG streaming):
  - `hand_gesture_recognition/`, `cat_detect/`, `dog_detect/`, `pedestrian_detect/`, `coco_detect/`, `human_face_detect/`, `coco_pose/`, `apriltag_recognition/`, `qrcode_recognition/`, `color_detect/`

## Build & Flash

```bash
cd hand_gesture_recognition
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
# Optional: export single merged binary at 0x0
idf.py merge-bin -o hand_gesture_recognition.bin
```

---

## 简体中文

本目录包含适用于 **ESP32-P4C5-CAM** 视觉模块的 **ESP-IDF v6.1** 全部 12 组源码工程与共享组件。

- `components/`：共享底层组件（摄像头、LCD、I²C 协议、Web 图传、BSP 等），**请务必与各例程目录同级保留**。
- 图像回传工程：`Image_transmission_function_AP`（纯 AP）、`Image_transmission_function_STA`（AP/STA 双模出厂源码）。
- 离线视觉工程（均支持 I²C `0x54` 输出与 Web 实时图传）：`hand_gesture_recognition`、`cat_detect`、`dog_detect`、`pedestrian_detect`、`coco_detect`、`human_face_detect`、`coco_pose`、`apriltag_recognition`、`qrcode_recognition`、`color_detect`。
- 使用 VS Code ESP-IDF 插件或命令行打开**单个例程子目录**，选择目标 `esp32p4` 即可直接编译烧录（首次构建将自动按 `dependencies.lock` 拉取托管组件）。
