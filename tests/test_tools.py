"""Unit tests for ESP32-P4C5-CAM repository developer tools: clean_repo, i2c_diagnose, and flash_firmware."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = ROOT / "tools"

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


class TestI2CDiagnose:
    """Test I2C diagnostic frame validation and mock mode."""

    def test_mock_diagnostic_suite(self):
        from i2c_diagnose import run_mock_test
        assert run_mock_test() is True

    def test_validate_frame_invalid_length(self):
        from i2c_diagnose import parse_and_validate_frame
        # Frame shorter than 32 bytes
        assert parse_and_validate_frame(b"\x00" * 16) is False

    def test_validate_frame_corrupted_checksum(self):
        from i2c_diagnose import parse_and_validate_frame
        frame = bytearray(32)
        frame[0] = 0xAA
        frame[1] = 0x55
        frame[2:4] = (4).to_bytes(2, "little")
        frame[4:6] = (1).to_bytes(2, "little")
        frame[6:8] = (1).to_bytes(2, "little")
        frame[8:12] = b"test"
        frame[12] = 0xFF  # Corrupted checksum
        assert parse_and_validate_frame(bytes(frame)) is False


class TestCleanRepo:
    """Test repository hygiene and cleanup script."""

    def test_dry_run_executes_cleanly(self):
        from clean_repo import clean
        assert clean(dry_run=True) == 0


class TestFlashFirmware:
    """Test firmware flasher tool discovery and command preview."""

    def test_list_serial_ports(self):
        from flash_firmware import list_serial_ports
        ports = list_serial_ports()
        assert isinstance(ports, list)

    def test_run_esptool_dry_run(self):
        from flash_firmware import run_esptool
        ret = run_esptool(
            port="COM1",
            bin_path=Path("dummy.bin"),
            chip="esp32p4",
            erase=True,
            baud=921600,
            dry_run=True,
        )
        assert ret == 0
