# Raspberry Pi Host Examples

**English** | [简体中文](#简体中文)

Python examples for interfacing a **Raspberry Pi (3B / 4B / 5 / CM4)** with the **ESP32-P4C5-CAM** vision module via Linux I²C bus.

---

## 40-Pin Header Wiring

| ESP32P4C5-Cam (`P1`) | Raspberry Pi 40-Pin Header | Notes |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | Pin 2 or Pin 4 (`5V`) | External 5V ≥ 2A supply recommended |
| **Pin 2 (GND)** | Pin 6 (`GND`) | Common ground |
| **Pin 3 (SDA)** | Pin 3 (`GPIO 2 / I2C1_SDA`) | 3.3V bus level |
| **Pin 4 (SCL)** | Pin 5 (`GPIO 3 / I2C1_SCL`) | 3.3V bus level |

---

## Environment Setup

1. Enable the I²C interface:
   ```bash
   sudo raspi-config
   # Select: Interface Options -> I2C -> Enable
   ```
2. Grant user access to the I²C bus:
   ```bash
   sudo usermod -aG i2c $USER
   ```
3. Test that device `0x54` (or `0x52` for streaming firmware) is detected on bus 1:
   ```bash
   sudo apt-get install -y i2c-tools
   i2cdetect -y 1
   ```
4. Run any example:
   ```bash
   cd 01_hand_gesture
   python3 main.py
   ```

---

## 简体中文

本目录包含适用于 **树莓派 (Raspberry Pi 3B / 4B / 5 / CM4)** 的 Python 视觉交互例程。

### 40-Pin 排针接线

| 模组端子 (`P1`) | 树莓派排针 | 说明 |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | 物理引脚 2 / 4 (`5V`) | 建议提供 **5V ≥ 2A** 稳定电源 |
| **Pin 2 (GND)** | 物理引脚 6 (`GND`) | 共地 |
| **Pin 3 (SDA)** | 物理引脚 3 (`GPIO 2 / SDA`) | I²C1 数据线 |
| **Pin 4 (SCL)** | 物理引脚 5 (`GPIO 3 / SCL`) | I²C1 时钟线 |

### 环境配置与运行

1. 开启树莓派 I²C 总线：
   ```bash
   sudo raspi-config
   # 选择：Interface Options -> I2C -> 开启
   ```
2. 赋予当前用户访问权限并检测总线从机（离线视觉固件默认地址为 `0x54`，图传固件为 `0x52`）：
   ```bash
   sudo usermod -aG i2c $USER
   sudo apt-get install -y i2c-tools
   i2cdetect -y 1
   ```
3. 进入对应案例目录直接运行：
   ```bash
   cd 01_hand_gesture
   python3 main.py
   ```
