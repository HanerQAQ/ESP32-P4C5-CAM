# NVIDIA Jetson Orin Host Examples

**English** | [简体中文](#简体中文)

Python examples for interfacing an **NVIDIA Jetson Orin (Nano / NX / AGX)** with the **ESP32-P4C5-CAM** vision module over Linux I²C bus.

---

## 40-Pin Header Wiring

| ESP32P4C5-Cam (`P1`) | Jetson Orin 40-Pin Header | Notes |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | Pin 2 or Pin 4 (`5V`) | External 5V ≥ 2A power recommended |
| **Pin 2 (GND)** | Pin 6 (`GND`) | Common ground |
| **Pin 3 (SDA)** | Pin 3 (`I2C2_SDA / GPIO 27`) | Onboard 3.3V bus level |
| **Pin 4 (SCL)** | Pin 5 (`I2C2_SCL / GPIO 28`) | Onboard 3.3V bus level |

---

## Environment Setup

1. Check that the I²C bus is available (typically `/dev/i2c-1` or `/dev/i2c-7` on Jetson Orin):
   ```bash
   sudo apt-get install -y i2c-tools
   sudo i2cdetect -y -r 1   # or 7
   ```
2. Grant user access to the I²C devices:
   ```bash
   sudo usermod -aG i2c $USER
   ```
3. Run any example (set bus number in `main.py` if different from default):
   ```bash
   cd 01_hand_gesture
   python3 main.py
   ```

---

## 简体中文

本目录包含适用于 **NVIDIA Jetson Orin (Nano / NX / AGX)** 的 Python 视觉交互例程。

### 40-Pin 排针接线

| 模组端子 (`P1`) | Jetson 40-Pin 排针 | 说明 |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | 物理引脚 2 / 4 (`5V`) | 模组需保证 **5V 2A** 供电 |
| **Pin 2 (GND)** | 物理引脚 6 (`GND`) | 共地 |
| **Pin 3 (SDA)** | 物理引脚 3 (`I2C2_SDA`) | 3.3V 电平 |
| **Pin 4 (SCL)** | 物理引脚 5 (`I2C2_SCL`) | 3.3V 电平 |

### 快速上手

1. 确认 Jetson 的 I²C 总线号（Orin Nano / NX 常见为 `/dev/i2c-1` 或 `/dev/i2c-7`）：
   ```bash
   sudo apt-get install -y i2c-tools
   sudo i2cdetect -y -r 1
   ```
2. 赋予免 sudo 访问权限：
   ```bash
   sudo usermod -aG i2c $USER
   ```
3. 进入案例目录并运行：
   ```bash
   cd 01_hand_gesture
   python3 main.py
   ```
