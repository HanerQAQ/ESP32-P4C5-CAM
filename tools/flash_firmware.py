#!/usr/bin/env python3
"""
Interactive ESP32-P4C5-CAM Firmware Flashing Tool.

Automatically scans available COM ports, locates compiled or prebuilt binaries,
and invokes esptool to write the firmware to flash at offset 0x0.

Usage:
    python tools/flash_firmware.py
    python tools/flash_firmware.py --port COMx --bin path/to/firmware.bin --erase
    python tools/flash_firmware.py --dry-run
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIR = ROOT / "firmware"
PREBUILT_DIR = ROOT / "prebuilt_firmware"

SUPPORTED_PROJECTS = [
    ("hand_gesture_recognition", "Hand Gesture Recognition / 手势识别"),
    ("cat_detect", "Cat Detection / 猫咪检测"),
    ("dog_detect", "Dog Detection / 狗狗检测"),
    ("pedestrian_detect", "Pedestrian Detection / 行人检测"),
    ("coco_detect", "YOLO11n COCO 80-Class Detection / YOLO11n 目标检测"),
    ("human_face_detect", "Human Face Detection / 人脸检测"),
    ("coco_pose", "Human Pose 17-Keypoint Detection / 人体姿态检测"),
    ("apriltag_recognition", "AprilTag 6-DoF Pose Recognition / AprilTag 识别"),
    ("qrcode_recognition", "QR Code Decoding / 二维码扫描识别"),
    ("color_detect", "Color Tracking & Line Following / 颜色识别与巡线"),
    ("Image_transmission_function_AP", "AP Video Streaming / AP 模式热点图传"),
    ("Image_transmission_function_STA", "STA Video Streaming / STA 网页配网图传"),
]


def ensure_esptool() -> bool:
    """Check if esptool is available; if not, prompt user and offer auto-install via pip."""
    try:
        res = subprocess.run(
            [sys.executable, "-m", "esptool", "version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if res.returncode == 0:
            return True
    except Exception:
        pass

    print("\n[!] 'esptool' is not installed in the current Python environment.")
    print("    esptool is required to flash firmware to ESP32-P4.")
    try:
        ans = input("    Would you like to install esptool automatically now? [Y/n]: ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        return False

    if ans in ("", "y", "yes"):
        print(f"\n[*] Running: {sys.executable} -m pip install esptool pyserial...")
        ret = subprocess.run([sys.executable, "-m", "pip", "install", "esptool", "pyserial"]).returncode
        if ret == 0:
            print("[OK] esptool successfully installed!\n")
            return True
        else:
            print("[ERROR] Failed to install esptool. Please run: pip install esptool")
            return False
    else:
        print("[!] Cannot proceed without esptool. Please install it manually: pip install esptool")
        return False


def list_serial_ports() -> list[tuple[str, str]]:
    """Scan and return available serial port identifiers."""
    ports = []
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            desc = f"{p.device} ({p.description})" if p.description else p.device
            ports.append((p.device, desc))
    except ImportError:
        if sys.platform.startswith("win"):
            for i in range(1, 30):
                ports.append((f"COM{i}", f"COM{i}"))
        else:
            for cand in ["/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyACM0", "/dev/ttyACM1"]:
                if os.path.exists(cand):
                    ports.append((cand, cand))
    return ports


def run_esptool(port: str, bin_path: Path, chip: str = "esp32p4", erase: bool = False, baud: int = 921600, dry_run: bool = False) -> int:
    """Invoke esptool to write binary to target ESP32-P4 module."""
    if not dry_run and not ensure_esptool():
        return 1
    py_exe = sys.executable

    if erase:
        cmd_erase = [
            py_exe, "-m", "esptool",
            "--chip", chip,
            "-p", port,
            "-b", str(baud),
            "erase_flash",
        ]
        print(f"\n[1/2] Erasing flash on {port}:")
        print("      " + " ".join(cmd_erase))
        if not dry_run:
            ret = subprocess.run(cmd_erase).returncode
            if ret != 0:
                print("[ERROR] Flash erase failed.")
                return ret

    cmd_write = [
        py_exe, "-m", "esptool",
        "--chip", chip,
        "-p", port,
        "-b", str(baud),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "-z",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "0x0", str(bin_path),
    ]

    print(f"\n[{'2/2' if erase else '1/1'}] Writing {bin_path.name} to {port} at offset 0x0:")
    print("      " + " ".join(cmd_write))

    if dry_run:
        print("[DRY-RUN] Command previewed successfully (no flash action taken).")
        return 0

    return subprocess.run(cmd_write).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32-P4C5-CAM Firmware Flashing Tool.")
    parser.add_argument("-p", "--port", type=str, help="Serial COM port (e.g. COM3 or /dev/ttyUSB0).")
    parser.add_argument("-b", "--bin", type=str, help="Path to binary file (.bin) to flash.")
    parser.add_argument("--chip", type=str, default="esp32p4", help="ESP target chip (default: esp32p4).")
    parser.add_argument("--baud", type=int, default=921600, help="Flashing baud rate (default: 921600).")
    parser.add_argument("--erase", action="store_true", help="Erase entire flash before writing.")
    parser.add_argument("--dry-run", action="store_true", help="Preview esptool commands without executing.")
    args = parser.parse_args()

    print("=" * 60)
    print("  ESP32-P4C5-CAM Firmware Flasher")
    print("=" * 60)

    # 1. Target Binary Selection
    bin_path = None
    if args.bin:
        bin_path = Path(args.bin).resolve()
        if not bin_path.is_file():
            print(f"[ERROR] Specified binary file not found: {bin_path}")
            return 1
    else:
        print("\nAvailable firmware projects in firmware/:")
        for idx, (proj, desc) in enumerate(SUPPORTED_PROJECTS, 1):
            build_bin = FIRMWARE_DIR / proj / "build" / f"{proj}.bin"
            status = f"[Built: {build_bin.name}]" if build_bin.is_file() else "[Source only]"
            print(f"  [{idx:2d}] {desc} {status}")

        if args.dry_run:
            print("[DRY-RUN] Mocking selection for project 1.")
            bin_path = FIRMWARE_DIR / SUPPORTED_PROJECTS[0][0] / "build" / f"{SUPPORTED_PROJECTS[0][0]}.bin"
        else:
            try:
                choice = input("\nSelect project to flash [1-12] or enter path to custom .bin: ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n[Exit] User cancelled.")
                return 0

            if choice.isdigit() and 1 <= int(choice) <= len(SUPPORTED_PROJECTS):
                proj_name = SUPPORTED_PROJECTS[int(choice) - 1][0]
                bin_path = FIRMWARE_DIR / proj_name / "build" / f"{proj_name}.bin"
                if not bin_path.is_file():
                    print(f"\n[!] Firmware binary for '{proj_name}' is not compiled yet.")
                    print(f"    Please build it first using ESP-IDF:")
                    print(f"      cd firmware/{proj_name}")
                    print(f"      idf.py set-target esp32p4")
                    print(f"      idf.py build")
                    return 1
            else:
                custom_p = Path(choice).resolve()
                if custom_p.is_file():
                    bin_path = custom_p
                else:
                    print(f"[ERROR] Invalid selection or file not found: {choice}")
                    return 1

    # 2. Port Selection
    port = args.port
    if not port:
        ports = list_serial_ports()
        if not ports:
            print("[ERROR] No serial ports found. Please connect the ESP32-P4 module via USB.")
            return 1

        if args.dry_run:
            port = ports[0][0]
        else:
            print("\nAvailable serial ports:")
            for idx, (p_id, p_desc) in enumerate(ports, 1):
                print(f"  [{idx}] {p_desc}")
            try:
                p_choice = input(f"Select port [1-{len(ports)}] (default: 1): ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n[Exit] User cancelled.")
                return 0
            if p_choice == "":
                port = ports[0][0]
            elif p_choice.isdigit() and 1 <= int(p_choice) <= len(ports):
                port = ports[int(p_choice) - 1][0]
            else:
                port = p_choice

    # 3. Flash Execution
    return run_esptool(
        port=port,
        bin_path=bin_path,
        chip=args.chip,
        erase=args.erase,
        baud=args.baud,
        dry_run=args.dry_run,
    )


if __name__ == "__main__":
    sys.exit(main())
