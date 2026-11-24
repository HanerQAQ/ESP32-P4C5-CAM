"""Automated tests to verify ESP32-P4C5-CAM firmware build configuration and optional release checksums."""

import hashlib
import os

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_PREBUILT_DIR = os.path.join(_REPO_ROOT, "prebuilt_firmware")
_CHECKSUMS_FILE = os.path.join(_PREBUILT_DIR, "SHA256SUMS.txt")
_FIRMWARE_DIR = os.path.join(_REPO_ROOT, "firmware")

EXPECTED_PROJECTS = [
    "Image_transmission_function_STA",
    "Image_transmission_function_AP",
    "hand_gesture_recognition",
    "cat_detect",
    "dog_detect",
    "pedestrian_detect",
    "coco_detect",
    "human_face_detect",
    "coco_pose",
    "apriltag_recognition",
    "qrcode_recognition",
    "color_detect",
]


def test_firmware_source_projects_present():
    """Verify all 12 ESP-IDF v6.1 firmware source projects and build script exist cleanly."""
    assert os.path.isfile(os.path.join(_PREBUILT_DIR, "README.md"))
    assert os.path.isfile(os.path.join(_FIRMWARE_DIR, "build_and_verify_all.py"))

    for proj in EXPECTED_PROJECTS:
        proj_dir = os.path.join(_FIRMWARE_DIR, proj)
        assert os.path.isdir(proj_dir), f"Missing firmware project: {proj}"
        assert os.path.isfile(os.path.join(proj_dir, "CMakeLists.txt")), f"Missing CMakeLists.txt in {proj}"
        assert os.path.isfile(os.path.join(proj_dir, "sdkconfig.defaults")), f"Missing sdkconfig.defaults in {proj}"


def test_prebuilt_firmware_sha256_checksums_when_present():
    """Verify SHA256SUMS.txt matches all binaries when release binaries are staged locally."""
    if not os.path.isfile(_CHECKSUMS_FILE):
        readme_text = open(os.path.join(_PREBUILT_DIR, "README.md"), encoding="utf-8").read()
        for proj in EXPECTED_PROJECTS:
            assert f"{proj}.bin" in readme_text, f"{proj}.bin missing from prebuilt_firmware/README.md"
        return

    with open(_CHECKSUMS_FILE, "r", encoding="utf-8") as f:
        lines = [line.strip() for line in f if line.strip() and not line.startswith("#")]

    for line in lines:
        expected_hash, fname = line.split(None, 1)
        bin_path = os.path.join(_PREBUILT_DIR, fname)
        assert os.path.isfile(bin_path), f"Firmware binary {fname} missing in {_PREBUILT_DIR}"
        hasher = hashlib.sha256()
        with open(bin_path, "rb") as bf:
            while chunk := bf.read(65536):
                hasher.update(chunk)
        assert hasher.hexdigest() == expected_hash
