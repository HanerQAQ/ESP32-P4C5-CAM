# Automated Test Suite

**English** | [简体中文](#简体中文)

Automated unit testing and hygiene verification suite for **ESP32-P4C5-CAM**.

## Test Coverage

- `test_driver.py`: Unit tests for the Python Host SDK driver (`ESP32_P4C5_CAM`):
  - Binary helpers (`_checksum`, `_u16_le`, `_float_le`)
  - I²C 32-byte frame validation and `0x55` resync recovery
  - Multi-fragment packet reassembly and error recovery
  - All 11 vision result decoders (Hand Gesture, Cat/Dog, Pedestrian, YOLO11n, Face, Pose, AprilTag, QR Code, Color, Line, Speech)
- `test_tools.py`: Tests for developer CLI utilities (`clean_repo.py`, `i2c_diagnose.py`, `flash_firmware.py`).
- `test_firmware_integrity.py`: Validates all 12 ESP-IDF v6.1 firmware projects and release binary checksums (when staged).
- `test_brand_hygiene.py`: Verifies zero proprietary/third-party vendor leftover brand strings across source files, docs, `.gz` archives, and schematics.
- `tools/color_tool/tests/test_protocol.py`: Tests ColorTool UART/Serial framing, HSV math, and factory presets.

## Running Tests

```bash
pytest -v
```

---

## 简体中文

本目录为 **ESP32-P4C5-CAM** 的自动化单元测试与仓库规范审计套件。

### 测试覆盖范围

- `test_driver.py`：Python 主控驱动单元测试（字节校验、32 字节分片组包、`0x55` 断帧自愈与全部 11 类视觉结果解码器）。
- `test_tools.py`：仓库开发工具测试（缓存清理、I²C 链路诊断仿真模式、固件烧录 CLI 预演）。
- `test_firmware_integrity.py`：校验全部 12 套 ESP-IDF v6.1 固件工程结构与构建清单完整性（发布版本时自动校对 `SHA256SUMS.txt`）。
- `test_brand_hygiene.py`：全仓库纯净度审计，确保源码、文档、网页压缩资源及图纸中不含遗留品牌标识。
- `tools/color_tool/tests/test_protocol.py`：ColorTool 调色上位机串口通信帧协议与 HSV 预设库测试。

### 本地运行

```bash
pytest -v
```
