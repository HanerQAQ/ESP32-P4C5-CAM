# Developer Tools & Utilities

**English** | [简体中文](#简体中文)

This directory provides developer utilities for maintaining, diagnosing, and flashing the **ESP32-P4C5-CAM** edge AI platform:

| Tool | Purpose | Usage Example |
| :--- | :--- | :--- |
| `flash_firmware.py` | Auto-detect COM ports and flash firmware to ESP32-P4 | `python tools/flash_firmware.py` |
| `i2c_diagnose.py` | Probe I²C connectivity, measure latency, verify framing | `python tools/i2c_diagnose.py --mock` |
| `clean_repo.py` | Scrub Python bytecode, `.pytest_cache`, and temporary builds | `python tools/clean_repo.py` |
| `color_tool/` | PySide6 HSV calibration software with factory presets | `python tools/color_tool/main.py` |

---

## 简体中文

本目录包含 **ESP32-P4C5-CAM** 边缘 AI 视觉模组的开发辅助工具与调试脚本：

| 工具 | 用途说明 | 常用命令 |
| :--- | :--- | :--- |
| `flash_firmware.py` | 自动识别串口并将目标固件烧录至 ESP32-P4 | `python tools/flash_firmware.py` |
| `i2c_diagnose.py` | I²C 连通性测试、往返延迟测量与帧校验诊断 | `python tools/i2c_diagnose.py --mock` |
| `clean_repo.py` | 仓库缓存深度清理工具（清除 `__pycache__`、`.pytest_cache`、临时编译文件） | `python tools/clean_repo.py` |
| `color_tool/` | 基于 PySide6 + OpenCV 的颜色识别与巡线阈值上位机调参软件 | `python tools/color_tool/main.py` |
