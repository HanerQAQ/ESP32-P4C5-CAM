# Host SDK & Multi-Platform Examples

**English** | [简体中文](#简体中文)

I²C host drivers and 11 vision examples for communicating with **ESP32-P4C5-CAM** over I²C (7-bit slave address `0x54` for offline vision firmwares, `400 kHz`).

## Platforms

| Folder | Target Platform | Language / Framework | Build Tool |
| :--- | :--- | :--- | :--- |
| [esp32_arduino/](esp32_arduino) | ESP32 boards | Arduino (`C++`) | Arduino IDE / PlatformIO |
| [esp32_micropython/](esp32_micropython) | ESP32 boards | MicroPython (`Python`) | mpremote / Thonny |
| [stm32/](stm32_hal) | STM32 MCUs | STM32 HAL + FreeRTOS (`C`) | CMake / STM32CubeIDE |
| [raspberry_pi/](raspberry_pi) | Raspberry Pi (3B/4B/5) | Python 3 (Native Linux `ioctl`) | Standard Python (Zero deps) |
| [jetson_orin/](jetson_orin) | NVIDIA Jetson Orin | Python 3 (Native Linux `ioctl`) | Standard Python (Zero deps) |

## Wiring Notes

1. Connect common `GND` between the host and camera module.
2. Supply **5V (`>= 2A`)** to `VCC`.
3. Connect `SDA` and `SCL` to the host hardware I²C pins.
4. Flash the matching ESP-IDF vision firmware onto the module before running a host example. See [../docs/i2c_protocol_en.md](../docs/i2c_protocol_en.md) for the protocol specification.

---

## 简体中文

本目录包含 **ESP32-P4C5-CAM** 作为 I²C 从机（离线视觉固件默认地址 `0x54`，图传固件地址 `0x52`，速率 `400 kHz`）时，与各主流主控平台通讯的驱动及 11 组视觉案例：

- **ESP32 Arduino** (`esp32_arduino/`)、`ESP32 MicroPython` (`esp32_micropython/`)
- **STM32 HAL + FreeRTOS** (`stm32_hal/`)
- **树莓派** (`raspberry_pi/`)、**Jetson Orin** (`jetson_orin/`)

运行前请确保模块已烧录对应功能的 ESP-IDF 视觉固件，且主控与模块共地并接入稳定 5V (`≥ 2A`) 供电；协议细节见 [../docs/i2c_protocol_zh.md](../docs/i2c_protocol_zh.md)。
