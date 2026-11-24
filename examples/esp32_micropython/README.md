# ESP32 MicroPython Host Examples



**English** | [简体中文](#简体中文)



This directory contains 11 MicroPython examples for communicating with the **ESP32-P4C5-CAM** vision module over I²C (`0x54`, 400 kHz).



---



## Wiring & Hardware



| ESP32-P4C5-CAM (`P1`) | ESP32 Board | Notes |

| :---: | :---: | :--- |

| **Pin 1 (5V / VCC)** | `5V / VIN` | Stable **5V ≥ 2A** required |

| **Pin 2 (GND)** | `GND` | Common ground |

| **Pin 3 (SDA)** | `GPIO 21` (Custom board: verify GPIO) | Hardware I2C SDA |

| **Pin 4 (SCL)** | `GPIO 22` (Custom board: verify GPIO) | Hardware I2C SCL |



---



## How to Run



1. Flash standard MicroPython firmware (v1.20+) onto your ESP32 board.

2. Open **Thonny IDE**, **rshell**, or **mpremote**.

3. Upload the unified driver `esp32_p4c5_cam.py` (from this directory) and your chosen example's `main.py` to the ESP32 root filesystem (or `/lib/esp32_p4c5_cam.py`).

4. Verify the I²C pin configuration in `main.py`:

   ```python

   from machine import Pin, I2C

   i2c = I2C(0, scl=Pin(22), sda=Pin(21), freq=400000)

   ```

5. Run `main.py` to observe real-time detection JSON output in the REPL.



---



## 简体中文



本目录包含适用于 **ESP32 MicroPython** 固件的 11 组视觉交互例程。



### 硬件接线



| 视觉模组端子 (`P1`) | ESP32 引脚 | 说明 |

| :---: | :---: | :--- |

| **Pin 1 (5V / VCC)** | `5V / VIN` | 稳定 **5V 2A** 供电 |

| **Pin 2 (GND)** | `GND` | 共地 |

| **Pin 3 (SDA)** | `GPIO 21` | I²C 数据线 |

| **Pin 4 (SCL)** | `GPIO 22` | I²C 时钟线 |



### 快速上手



1. 为 ESP32 烧录 MicroPython 官方固件（v1.20+）。

2. 使用 **Thonny IDE** 连接 ESP32。

3. 将本目录的统一驱动文件 `esp32_p4c5_cam.py` 与目标案例目录下的 `main.py` 上传至开发板文件系统（或存放于 `/lib/esp32_p4c5_cam.py`）。

4. 运行 `main.py`，在终端查看解析后的识别结果与坐标数据。

