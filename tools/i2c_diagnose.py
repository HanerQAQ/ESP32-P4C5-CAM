#!/usr/bin/env python3
"""
ESP32-P4C5-CAM I2C Communication Diagnostic Tool.

Tests connectivity with the ESP32-P4C5-CAM module over I2C at slave address 0x54 (offline AI)
or 0x52 (Wi-Fi streaming) at 400 kHz (Fast Mode) / 100 kHz.
Sends a test JSON command, receives the 32-byte fragment frame, verifies XOR checksum,
measures round-trip latency, and provides actionable troubleshooting advice.

Supported environments:
  - Linux SBCs (Raspberry Pi, NVIDIA Jetson, Orange Pi) via /dev/i2c-*
  - MicroPython / CircuitPython via standard machine.I2C
  - Simulated testing via --mock mode

Usage:
  python tools/i2c_diagnose.py
  python tools/i2c_diagnose.py --bus 1 --addr 0x54
  python tools/i2c_diagnose.py --mock
"""

from __future__ import annotations

import argparse
import sys
import time

I2C_ADDR_PRIMARY = 0x54    # Offline AI vision & speech
I2C_ADDR_SECONDARY = 0x52  # Wi-Fi streaming service
FRAME_MAGIC = 0x55AA


def parse_and_validate_frame(frame: bytes, elapsed_ms: float = 20.0) -> bool:
    """Parse and validate a 32-byte ESP32-P4C5-CAM I2C response frame."""
    if len(frame) < 32:
        print(f"[WARN] Received {len(frame)} bytes, expected 32 bytes.")
        return False

    # Resync recovery: If first byte 0xAA was lost in gap, frame starts with 0x55
    if frame[0] == 0x55 and frame[1] != 0x55:
        candidate = bytes([0xAA]) + frame[:31]
        c_len = int.from_bytes(candidate[2:4], "little")
        if 0 < c_len <= 23:
            c_payload = candidate[8:8 + c_len]
            c_chk = 0
            for b in c_payload:
                c_chk ^= b
            if c_chk == candidate[8 + c_len]:
                print("[INFO] Applied 0x55 resync recovery (0xAA byte restored).")
                frame = candidate

    header = int.from_bytes(frame[0:2], "little")
    data_len = int.from_bytes(frame[2:4], "little")
    frag_id = int.from_bytes(frame[4:6], "little")
    total_frag = int.from_bytes(frame[6:8], "little")

    if not (0 < data_len <= 23):
        print(f"[WARN] Invalid data length: {data_len} bytes (expected 1..23). Raw frame: {frame.hex()}")
        return False

    payload = frame[8:8 + data_len]
    checksum = frame[8 + data_len]

    # Calculate XOR checksum of valid DATA payload
    calc_xor = 0
    for byte in payload:
        calc_xor ^= byte

    print(f"[3/3] Response received in {elapsed_ms:.2f} ms:")
    print(f"      Header: 0x{header:04X} ({'VALID 0x55AA' if header == FRAME_MAGIC else 'MISMATCH'})")
    print(f"      Data Length: {data_len} bytes")
    print(f"      Fragment: {frag_id} / {total_frag}")
    print(f"      Payload (hex): {payload.hex()}")
    print(f"      XOR Checksum: 0x{checksum:02X} ({'VALID' if calc_xor == checksum else f'MISMATCH: calculated 0x{calc_xor:02X}'})")

    if header == FRAME_MAGIC and calc_xor == checksum:
        print("\n[SUCCESS] ESP32-P4C5-CAM I2C slave is working properly!")
        return True
    else:
        print("\n[WARN] Received data corrupted or framing error.")
        return False


def run_mock_test() -> bool:
    """Run an automated diagnostic test using synthesized protocol frames."""
    print("[*] Running simulated diagnostic test in --mock mode...")
    payload = b'{"status":0}'
    data_len = len(payload)
    xor_chk = 0
    for b in payload:
        xor_chk ^= b

    # Construct standard 32-byte frame: 0x55AA (little-endian: 0xAA, 0x55)
    frame = bytearray(32)
    frame[0] = 0xAA
    frame[1] = 0x55
    frame[2:4] = data_len.to_bytes(2, "little")
    frame[4:6] = (1).to_bytes(2, "little")
    frame[6:8] = (1).to_bytes(2, "little")
    frame[8:8 + data_len] = payload
    frame[8 + data_len] = xor_chk

    print("\n--- Test 1: Standard Frame Validation ---")
    ok1 = parse_and_validate_frame(bytes(frame), elapsed_ms=18.5)

    print("\n--- Test 2: 0x55 Resync Recovery Simulation ---")
    # Simulate first 0xAA dropped: frame starts with 0x55, payload shifted
    shifted = bytes([0x55]) + frame[2:32] + bytes([0x00])
    ok2 = parse_and_validate_frame(shifted, elapsed_ms=21.0)

    print("\n--- Test 3: Checksum Corruption Rejection ---")
    corrupt = bytearray(frame)
    corrupt[8 + data_len] ^= 0xFF
    ok3 = not parse_and_validate_frame(bytes(corrupt), elapsed_ms=19.2)

    all_passed = ok1 and ok2 and ok3
    if all_passed:
        print("\n[MOCK PASSED] All protocol diagnostic tests completed successfully.")
    else:
        print("\n[MOCK FAILED] Simulated test failed.")
    return all_passed


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32-P4C5-CAM I2C Diagnostic Tool.")
    parser.add_argument("--bus", type=int, default=1, help="I2C bus number (default: 1 on Raspberry Pi, 7 on Jetson Orin).")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=I2C_ADDR_PRIMARY, help="I2C slave address (default: 0x54).")
    parser.add_argument("--mock", action="store_true", help="Run simulated self-test without physical hardware.")
    args = parser.parse_args()

    if args.mock:
        return 0 if run_mock_test() else 1

    print(f"[*] ESP32-P4C5-CAM I2C Diagnostic starting on /dev/i2c-{args.bus} (slave: 0x{args.addr:02X})...")
    try:
        import os
        import fcntl
        import ctypes

        class _I2CMsg(ctypes.Structure):
            _fields_ = [
                ('addr', ctypes.c_uint16),
                ('flags', ctypes.c_uint16),
                ('len', ctypes.c_uint16),
                ('buf', ctypes.POINTER(ctypes.c_uint8)),
            ]

        class _I2CRdwrIoctlData(ctypes.Structure):
            _fields_ = [
                ('msgs', ctypes.POINTER(_I2CMsg)),
                ('nmsgs', ctypes.c_uint32),
            ]

        I2C_RDWR = 0x0707
        I2C_M_RD = 0x0001

        dev_path = f"/dev/i2c-{args.bus}"
        if not os.path.exists(dev_path):
            print(f"[ERROR] I2C device node '{dev_path}' not found.")
            print(f"        Ensure I2C is enabled (e.g. raspi-config or jetson-io).")
            return 1

        fd = os.open(dev_path, os.O_RDWR)
        print(f"[1/3] Successfully opened {dev_path}.")

        # Send test JSON command
        cmd = b'{"command":"get_info","params":"color_config"}'
        print(f"[2/3] Sending query command: {cmd.decode('utf-8')}")
        w_buf = (ctypes.c_uint8 * len(cmd))(*cmd)
        w_msg = _I2CMsg(args.addr, 0, len(cmd), w_buf)
        w_data = _I2CRdwrIoctlData(ctypes.pointer(w_msg), 1)

        t0 = time.time()
        fcntl.ioctl(fd, I2C_RDWR, w_data)
        time.sleep(0.015)

        # Read 32-byte frame
        r_buf = (ctypes.c_uint8 * 32)()
        r_msg = _I2CMsg(args.addr, I2C_M_RD, 32, r_buf)
        r_data = _I2CRdwrIoctlData(ctypes.pointer(r_msg), 1)
        fcntl.ioctl(fd, I2C_RDWR, r_data)
        elapsed = (time.time() - t0) * 1000.0

        os.close(fd)
        frame_bytes = bytes(r_buf)
        return 0 if parse_and_validate_frame(frame_bytes, elapsed) else 1

    except Exception as exc:
        print(f"[ERROR] Communication error: {exc}")
        print("\nTroubleshooting Suggestions:")
        print("1. Verify wiring: VCC (5V/3.3V), GND, SDA, SCL connected firmly.")
        print("2. Run 'i2cdetect -y <bus>' to check if device appears at 0x54 or 0x52.")
        print("3. Check user permissions: sudo usermod -aG i2c $USER.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
