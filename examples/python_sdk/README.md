# ESP32-P4C5-CAM Linux Python SDK / Linux 单板机 Python 驱动库

Unified Python driver for communicating with the **ESP32-P4C5-CAM** module over I²C (`0x54`, `400 kHz`) on Linux single-board computers (**Raspberry Pi**, **NVIDIA Jetson Orin/Xavier/Nano**, **Orange Pi**, **Rock Pi**).

适用于 Linux 单板计算机（树莓派全系列、NVIDIA Jetson Orin/Xavier/Nano、香橙派等）的统一 Python I²C 通信驱动库，基于 Linux 内核 `I2C_RDWR` ioctl 实现纯二进制直连通信，零第三方依赖。

---

## Installation / 安装

```bash
cd examples/python_sdk
pip install .

# Or editable install / 或可编辑安装：
pip install -e .
```

---

## Quick Example / 快速调用示例

```python
from esp32_p4c5_cam import ESP32_P4C5_CAM, ESP32_P4C5_CAM_I2C_ADDR, RawI2C

with RawI2C() as i2c:
    cam = ESP32_P4C5_CAM(i2c, addr=ESP32_P4C5_CAM_I2C_ADDR)
    gestures = cam.get_hand_gesture_result()
    print("Detected gestures:", gestures["items"])
```

---

## Hardware Pinout Reference / 硬件接线

| ESP32-P4C5-CAM (`P1`) | Raspberry Pi 40-Pin | Jetson Orin 40-Pin | Notes |
| :---: | :---: | :---: | :--- |
| **Pin 1 (`5V / VCC`)** | Pin 2 or 4 (`5V`) | Pin 2 or 4 (`5V`) | Stable `DC 5V >= 2A` supply |
| **Pin 2 (`GND`)** | Pin 6 (`GND`) | Pin 6 (`GND`) | Common ground (required) |
| **Pin 3 (`SDA`)** | Pin 3 (`GPIO2 / I2C1_SDA`) | Pin 3 (`I2C2_SDA / /dev/i2c-7`) | 3.3V I²C Data |
| **Pin 4 (`SCL`)** | Pin 5 (`GPIO3 / I2C1_SCL`) | Pin 5 (`I2C2_SCL / /dev/i2c-7`) | 3.3V I²C Clock |
