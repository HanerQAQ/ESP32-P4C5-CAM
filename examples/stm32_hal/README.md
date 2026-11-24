# STM32 ESP32-P4C5-CAM I²C Driver & Examples

**English** | [简体中文](#简体中文)

Minimal **STM32 HAL (FreeRTOS / Bare-Metal)** driver and 11 application examples. All hardware-specific robot peripherals have been decoupled, allowing direct integration into any standard STM32CubeMX project (STM32F1/F4/H7/G4, I²C address `0x54`, 400 kHz).

## Hardware Wiring

Connect the carrier board `P1 (5264-4P)` header to your STM32 development board:

| ESP32P4C5-Cam (`P1`) | STM32 Board (e.g. F401 / F411 / F103) | Notes |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | `5V` | Stable **5V ≥ 2A** supply recommended |
| **Pin 2 (GND)** | `GND` | Common ground |
| **Pin 3 (SDA)** | `PB7 / PB9 / PB11` (I²C SDA) | 3.3V bus level (with 3.3V pull-up) |
| **Pin 4 (SCL)** | `PB6 / PB8 / PB10` (I²C SCL) | 3.3V bus level (with 3.3V pull-up) |

---

## Structure

- `driver/`: Shared I²C driver and HAL porting layer
  - `ESP32_P4C5_CAM.h` / `ESP32_P4C5_CAM.c`: 32-byte fragmented frame parser, JSON command builder, and vision APIs
  - `ESP32_P4C5_CAM_port.h` / `ESP32_P4C5_CAM_port.c`: STM32 HAL `I2C_HandleTypeDef` read/write porting layer
- `demos/`: `app.c` application logic for all 11 vision cases (`01_hand_gesture` – `11_line_track`)

## Integration

1. Enable one I²C peripheral (`Fast Mode 400 kHz`, 7-bit addressing) and a debug UART in STM32CubeMX.
2. Add the 4 source files in `driver/` to your project and configure the target `I2C_HandleTypeDef` in `ESP32_P4C5_CAM_port.h` (defaults to `hi2c2`; can be overridden by defining `ESP32_P4C5_CAM_I2C_HANDLE`).
3. Reference the initialization and polling logic in `demos/<case>_app.c`.

---

## 简体中文

本目录包含适用于 **STM32 HAL（兼容 FreeRTOS / 裸机系统）** 的 `ESP32-P4C5-CAM` 视觉模块核心驱动及 11 个视觉案例应用代码（已彻底解耦机器人底盘与舵机依赖，从机地址 `0x54`，速率 `400 kHz`，可直接接入任意标准 STM32CubeMX 工程）。

### 硬件接线

| 视觉模组端子 (`P1`) | STM32 开发板（如 F401 / F411 / F103） | 说明 |
| :---: | :---: | :--- |
| **Pin 1 (5V / VCC)** | `5V` | 请确保供电能力至少 **5V 2A** |
| **Pin 2 (GND)** | `GND` | 共地 |
| **Pin 3 (SDA)** | `PB7 / PB9 / PB11` (I²C SDA) | 3.3V 电平（需具备 3.3V 上拉） |
| **Pin 4 (SCL)** | `PB6 / PB8 / PB10` (I²C SCL) | 3.3V 电平（需具备 3.3V 上拉） |

### 快速上手与工程接入

1. 在 STM32CubeMX 工程中开启一路 I²C 外设（Fast Mode `400 kHz`，7-bit 地址模式）及串口调试输出。
2. 将 `driver/` 下的 `ESP32_P4C5_CAM.c/.h` 与 `ESP32_P4C5_CAM_port.c/.h` 加入工程编译路径，并在 `ESP32_P4C5_CAM_port.h` 中配置目标 I²C 句柄宏 `ESP32_P4C5_CAM_I2C_HANDLE`（默认为 `hi2c2`）。
3. 参考 `demos/<对应案例>_app.c` 中的初始化与轮询解析逻辑完成集成。
