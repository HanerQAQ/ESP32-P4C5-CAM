# ESP32 Arduino Host Examples

**English** | [简体中文](#简体中文)

This directory contains 11 Arduino examples for communicating with the **ESP32-P4C5-CAM** vision module over I²C (slave address `0x54`, `400 kHz`).

---

## Hardware Connections

Connect the carrier board `P1 (5264-4P)` header to your ESP32 Arduino host:

| ESP32-P4C5-CAM (`P1`) | ESP32 Development Board | Notes |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | `5V / VIN` | Stable **5V ≥ 2A** supply required |
| **Pin 2 (GND)** | `GND` | Common ground |
| **Pin 3 (SDA)** | `GPIO 21` (default ESP32 SDA) | External 3.3V pull-up if needed |
| **Pin 4 (SCL)** | `GPIO 22` (default ESP32 SCL) | External 3.3V pull-up if needed |

*Note: For ESP32-S3 boards, default I²C pins are typically SDA = GPIO 8, SCL = GPIO 9. Modify `Wire.begin(SDA, SCL)` accordingly.*

---

## Library Structure & How to Run

This folder is a standard **Arduino 1.5 Library**:
- `ESP32_P4C5_CAM/src/`: Shared driver core (`ESP32_P4C5_CAM.h`, `ESP32_P4C5_CAM.cpp`).
- `ESP32_P4C5_CAM/examples/`: 11 ready-to-run sketches (`examples/01_hand_gesture/` through `examples/11_line_track/`).

### Usage Options

- **Option A (Recommended - IDE Examples Menu)**:
  Copy or symlink this folder (`examples/esp32_arduino/ESP32_P4C5_CAM`) into your Arduino `libraries/` directory as `ESP32-P4C5-CAM`.
  In Arduino IDE, select **File -> Examples -> ESP32_P4C5_CAM** to open any example directly.
- **Option B (Direct Open)**:
  Open any sketch under `examples/<case>/<case>.ino` directly. Ensure `ESP32-P4C5-CAM` is in your libraries path.
- **Option C (PlatformIO CLI)**:
  Run `pio run -e 01_hand_gesture` using [`platformio.ini`](platformio.ini) (update `src_dir` in `platformio.ini` to build another example `02` ~ `11`).

### Step-by-Step

1. Open the Arduino IDE (v2.x recommended).
2. Install the **esp32 by Espressif Systems** board package via Boards Manager.
3. Select your ESP32 board and COM port.
4. Open an example from **File -> Examples -> ESP32_P4C5_CAM** (e.g. `01_hand_gesture`).
5. Ensure the camera module has been flashed with the matching ESP-IDF vision firmware.
6. Click **Upload** and open the Serial Monitor at **115200 baud**.

> [!NOTE]
> **Arduino-ESP32 Core Compatibility**: Recommended board package is **esp32 by Espressif Systems v2.0.14+** for clean zero-warning builds. If compiling under Arduino-ESP32 Core v3.x, legacy ESP-IDF `driver/i2c.h` calls produce a non-fatal deprecation warning that does not affect runtime I²C operations.

---

## 简体中文

本目录包含适用于 **ESP32 Arduino** 主控的 11 组视觉交互例程，通过 I²C 总线（从机地址 `0x54`，推荐速率 `400 kHz`）与 **ESP32-P4C5-CAM** 视觉模块通信。

### 硬件接线

| 视觉模组端子 (`P1`) | ESP32 开发板 | 说明 |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | `5V / VIN` | 请确保供电能力至少 **5V 2A** |
| **Pin 2 (GND)** | `GND` | 共地 |
| **Pin 3 (SDA)** | `GPIO 21` (标准 ESP32 SDA) | 外部具备 3.3V 弱上拉 |
| **Pin 4 (SCL)** | `GPIO 22` (标准 ESP32 SCL) | 外部具备 3.3V 弱上拉 |

*注：若使用 ESP32-S3 等其他型号板卡，请在代码中的 `Wire.begin(SDA_PIN, SCL_PIN)` 配置实际使用的 GPIO 引脚。*

### 库结构与使用方式

本目录符合标准 **Arduino 1.5 Library** 规范：
- `ESP32_P4C5_CAM/src/`：底层共享驱动源码（`ESP32_P4C5_CAM.h`、`ESP32_P4C5_CAM.cpp`）。
- `ESP32_P4C5_CAM/examples/`：11 组开箱即用视觉例程（`examples/01_hand_gesture/` 至 `examples/11_line_track/`）。

#### 推荐使用方式

1. **导入为 Arduino 标准库（推荐）**：
   将本目录（`examples/esp32_arduino/ESP32_P4C5_CAM`）复制或软链接至 Arduino 的 `libraries/ESP32_P4C5_CAM` 路径；
   在 Arduino IDE 菜单栏直接选择 **文件 -> 示例 -> ESP32_P4C5_CAM**，即可一键打开任意例程。
2. **直接打开编译**：
   直接双击打开 `examples/<案例名>/<案例名>.ino` 源码文件进行编译上传。
3. **PlatformIO 命令行编译**：
   在当前目录执行 `pio run -e 01_hand_gesture`（通过 [`platformio.ini`](platformio.ini) 默认构建 `01_hand_gesture`，修改其中 `src_dir` 即可切换至 `02` ~ `11` 案例）。

### 快速上手

1. 打开 Arduino IDE，通过开发板管理器安装 `esp32 by Espressif Systems` 支持包。
2. 在 **文件 -> 示例 -> ESP32_P4C5_CAM** 中打开目标例程（如 `01_hand_gesture`）。
3. 确保视觉模组已烧录对应的 ESP-IDF 视觉识别固件。
4. 编译上传至 ESP32 开发板，打开串口监视器（波特率 `115200`）查看实时识别与数据解析输出。

> [!NOTE]
> **开发板支持包建议**：推荐安装 **esp32 by Espressif Systems v2.0.14+** 稳定版本（编译零警告）。若在最新的 Arduino-ESP32 Core v3.x 下编译，底层驱动调用的 `driver/i2c.h` 可能会提示弃用警告，该提示不影响正常运行。
