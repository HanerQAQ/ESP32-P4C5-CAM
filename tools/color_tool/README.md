# ColorTool v1.0 (Color Threshold Calibration Utility)

**English** | [简体中文](#简体中文)

A desktop GUI utility built with **PySide6** and **OpenCV** for calibrating HSV/color thresholds and flashing color configurations to the **ESP32-P4C5-CAM** module running the `color_detect` firmware over USB serial.

<div align="center">
  <img src="../../docs/assets/color_tool_ui.png" alt="ColorTool v1.0 UI" width="680"/>
</div>

## Features

- Live camera preview & interactive HSV color picker
- Read / write multi-color threshold presets via serial protocol (see [Protocol Specification](../../docs/uart_protocol_en.md))
- Real-time threshold mask visualization for color blob & line tracking
- Fully integrated with the [color_detect firmware](../../firmware/color_detect/README.md)

## Prebuilt Executable (Recommended for Windows)

Precompiled standalone executables (`ColorTool_v1.0_Windows_x64.zip`) are available on the repository's **[GitHub Releases](https://github.com/HanerQAQ/ESP32-P4C5-CAM/releases)** page with SHA-256 checksums. Download and extract to run `ColorTool_v1.0.exe` directly without installing Python or external dependencies.

## Run from Source

### Windows

```bash
pip install -r requirements.txt
python main.py
```

You can also double-click `run.bat`, or package a standalone executable with PyInstaller:

```bash
pip install pyinstaller
build_exe.bat
```

### Linux (Ubuntu / Debian) & macOS

Ensure system OpenGL/EGL libraries are present for PySide6:

```bash
# Ubuntu / Debian
sudo apt-get update && sudo apt-get install -y libgl1 libegl1
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
./run.sh   # or: python3 main.py
```

---

## 简体中文

基于 **PySide6** 与 **OpenCV** 开发的 **ESP32-P4C5-CAM** 颜色阈值上位机调节工具源码（配合 `color_detect` 固件使用）。

### 主要功能

- 支持串口连接视觉模块、实时预览与鼠标点选取色
- 支持多组颜色阈值的读取、调节、Mask 二值化效果预览与一键下发保存
- 协议规范详解见 [串口通信协议文档](../../docs/uart_protocol_zh.md)
- 固件端源码见 [color_detect 例程](../../firmware/color_detect/README.md)

### 免安装预编译运行（推荐 Windows 用户）

项目在 **[GitHub Releases](https://github.com/HanerQAQ/ESP32-P4C5-CAM/releases)** 发布页中提供了自动构建的独立绿色免安装包 `ColorTool_v1.0_Windows_x64.zip`（附带 SHA256 校验和）。非 Python 开发者下载解压后即可直接双击运行 `ColorTool_v1.0.exe`，无需配置 Python 依赖。

### 源码运行与打包

#### Windows

```bash
pip install -r requirements.txt
python main.py
```

Windows 下可直接运行 `run.bat`，或安装 `pyinstaller` 后执行 `build_exe.bat` 生成独立 `.exe` 可执行文件。

#### Linux (Ubuntu) 与 macOS

在 Linux 下需确保安装 Qt GUI 依赖的 OpenGL/EGL 库：

```bash
sudo apt-get update && sudo apt-get install -y libgl1 libegl1
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
./run.sh   # 或直接运行 python3 main.py
```
