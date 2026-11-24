# -*- coding: utf-8 -*-
"""
Automated unit tests for ESP32-P4C5-CAM Python Host SDK driver.
Covers:
- Helper functions (_checksum, _u16_le, _float_le, _sleep_ms)
- ESP32_P4C5_CAM_Error exception formatting
- Frame verification (checksum, header, size, fragments)
- Multi-fragment packet assembly and error recovery
- Vision result decoders:
  * Hand gesture recognition
  * Target detection (Cat, Dog, Pedestrian, YOLO11n, Face)
  * Pose detection (17 keypoints)
  * AprilTag 6-DoF pose estimation
  * QR Code decoder (UTF-8 and fallback encoding)
  * Color recognition and line tracking
  * Speech recognition and voice prompt commands
"""

import os
import sys
import struct
import pytest

# Add examples/python_sdk to sys.path
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SDK_DIR = os.path.join(_REPO_ROOT, 'examples', 'python_sdk')
if _SDK_DIR not in sys.path:
    sys.path.insert(0, _SDK_DIR)

from esp32_p4c5_cam.driver import (
    ESP32_P4C5_CAM,
    ESP32_P4C5_CAM_Error,
    ESP32_P4C5_CAM_I2C_ADDR,
    VISION_MODE_COLOR,
    VISION_MODE_LINE,
    ESP32_P4C5_CAM_MAX_COLOR_BLOCKS,
    MAX_COLOR_BLOCKS,
    MAX_TARGET_COLORS,
    HAND_GESTURE_MAX_COUNT,
    TARGET_DETECTION_MAX_COUNT,
    YOLO11N_CLASS_MAX_ID,
    POSE_MAX_COUNT,
    POSE_KEYPOINT_COUNT,
    APRILTAG_MAX_COUNT,
    QRCODE_MAX_COUNT,
    QRCODE_MAX_TEXT_LEN,
    HAND_GESTURE_DISLIKE,
    _FRAME_HEADER_H,
    _FRAME_HEADER_L,
    _FRAME_MAX_SIZE,
    _MAX_FRAME_DATA_SIZE,
    _checksum,
    _u16_le,
    _float_le,
)


class MockI2C:
    """Mock I2C bus providing readfrom and writeto methods."""

    def __init__(self, read_queue=None):
        self.read_queue = list(read_queue or [])
        self.written_data = []

    def writeto(self, addr, data):
        self.written_data.append((addr, bytes(data)))

    def readfrom(self, addr, count):
        if not self.read_queue:
            return bytes([0xFF] * count)
        chunk = self.read_queue.pop(0)
        return chunk


def make_frame(payload: bytes, fragment_id: int = 1, total_fragments: int = 1) -> bytes:
    """Synthesizes a compliant 32-byte slave I2C response frame."""
    data_len = len(payload)
    if data_len > _MAX_FRAME_DATA_SIZE:
        raise ValueError("Payload exceeds max frame data size (%d)" % _MAX_FRAME_DATA_SIZE)
    header = bytes([
        _FRAME_HEADER_H,
        _FRAME_HEADER_L,
        data_len & 0xFF,
        (data_len >> 8) & 0xFF,
        fragment_id & 0xFF,
        (fragment_id >> 8) & 0xFF,
        total_fragments & 0xFF,
        (total_fragments >> 8) & 0xFF,
    ])
    chk = _checksum(payload)
    pad_len = _FRAME_MAX_SIZE - len(header) - data_len - 1
    return header + payload + bytes([chk]) + bytes([0x00] * pad_len)


# ----------------------------------------------------------------------
# 1. Binary Helpers & Error Tests
# ----------------------------------------------------------------------
def test_checksum():
    assert _checksum(b'') == 0
    assert _checksum(b'\xAA') == 0xAA
    assert _checksum(b'\x01\x02\x03') == (1 ^ 2 ^ 3)
    assert _checksum(bytes([0xFF, 0xFF])) == 0


def test_u16_le():
    data = bytes([0x34, 0x12, 0x78, 0x56])
    assert _u16_le(data, 0) == 0x1234
    assert _u16_le(data, 2) == 0x5678


def test_float_le():
    packed = struct.pack('<f', 3.14159)
    recovered = _float_le(packed, 0)
    assert abs(recovered - 3.14159) < 1e-4


def test_error_str():
    err = ESP32_P4C5_CAM_Error('invalid_size', 'Packet too short')
    assert str(err) == 'Packet too short(invalid_size)'
    assert err.code == 'invalid_size'
    assert err.message == 'Packet too short'


# ----------------------------------------------------------------------
# 2. Driver Init & Basic Command Sending
# ----------------------------------------------------------------------
def test_driver_init():
    mock = MockI2C()
    driver = ESP32_P4C5_CAM(mock)
    assert driver.addr == ESP32_P4C5_CAM_I2C_ADDR

    driver_custom = ESP32_P4C5_CAM(mock, addr=0x52)
    assert driver_custom.addr == 0x52


def test_send_json():
    mock = MockI2C()
    driver = ESP32_P4C5_CAM(mock)
    driver.send_json('{"test":1}')
    assert len(mock.written_data) == 1
    assert mock.written_data[0][0] == ESP32_P4C5_CAM_I2C_ADDR
    assert mock.written_data[0][1] == b'{"test":1}'

    with pytest.raises(ESP32_P4C5_CAM_Error) as excinfo:
        driver.send_json('')
    assert excinfo.value.code == 'invalid_arg'


def test_play_voice_prompt():
    mock = MockI2C()
    driver = ESP32_P4C5_CAM(mock)

    driver.play_voice_prompt(1, 2)
    expected_param = (1 << 8) | 2
    assert mock.written_data[-1][1] == ('{"command":"play_voice_prompt","params":%d}' % expected_param).encode('utf-8')

    with pytest.raises(ESP32_P4C5_CAM_Error):
        driver.play_voice_prompt(-1, 0)
    with pytest.raises(ESP32_P4C5_CAM_Error):
        driver.play_voice_prompt(0, 256)


# ----------------------------------------------------------------------
# 3. Frame Reception & Multi-fragment Packet Assembly
# ----------------------------------------------------------------------
def test_receive_frame_success():
    payload = b'\x01\x02\x03\x04\x05'
    raw = make_frame(payload, fragment_id=1, total_fragments=1)
    mock = MockI2C([raw])
    driver = ESP32_P4C5_CAM(mock)

    d_len, fid, tf, p = driver.receive_frame()
    assert d_len == len(payload)
    assert fid == 1
    assert tf == 1
    assert p == payload


def test_receive_frame_errors():
    mock = MockI2C([b'\x00' * 16])
    driver = ESP32_P4C5_CAM(mock)
    with pytest.raises(ESP32_P4C5_CAM_Error) as exc:
        driver.receive_frame()
    assert exc.value.code == 'invalid_size'

    # Bad header
    bad_header = b'\x00\x00' + bytes([0x00] * 30)
    mock = MockI2C([bad_header])
    driver = ESP32_P4C5_CAM(mock)
    with pytest.raises(ESP32_P4C5_CAM_Error) as exc:
        driver.receive_frame()
    assert exc.value.code == 'invalid_response'

    # Checksum mismatch
    valid = bytearray(make_frame(b'\x01\x02\x03'))
    valid[8 + 3] ^= 0xFF  # Corrupt checksum byte
    mock = MockI2C([bytes(valid)])
    driver = ESP32_P4C5_CAM(mock)
    with pytest.raises(ESP32_P4C5_CAM_Error) as exc:
        driver.receive_frame()
    assert exc.value.code == 'invalid_checksum'


def test_receive_packet_multi_fragment():
    payload1 = b'ABCDEFGHIJ'
    payload2 = b'KLMNOPQRST'
    frame1 = make_frame(payload1, fragment_id=1, total_fragments=2)
    frame2 = make_frame(payload2, fragment_id=2, total_fragments=2)

    mock = MockI2C([frame1, frame2])
    driver = ESP32_P4C5_CAM(mock)
    packet = driver.receive_packet()
    assert packet == payload1 + payload2


def test_receive_packet_fragment_discontinuity():
    f1 = make_frame(b'A', fragment_id=1, total_fragments=2)
    f2 = make_frame(b'B', fragment_id=1, total_fragments=2)  # repeated 1 instead of 2
    mock = MockI2C([f1, f2])
    driver = ESP32_P4C5_CAM(mock)
    with pytest.raises(ESP32_P4C5_CAM_Error) as exc:
        driver.receive_packet()
    assert exc.value.code == 'invalid_response'


# ----------------------------------------------------------------------
# 4. Vision Result Decoders
# ----------------------------------------------------------------------
def test_speech_recognition_result():
    mock = MockI2C([make_frame(bytes([0x12, 0x34]))])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_speech_recognition_result()
    assert res == {'data0': 0x12, 'data1': 0x34}


def test_color_config():
    # mode = 0 (COLOR), targets = [1, 2, 3]
    mock = MockI2C([make_frame(bytes([0x00, 0x01, 0x02, 0x03]))])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_color_config()
    assert res['mode'] == 0
    assert res['registered_color_count'] == 3
    assert res['targets'] == [1, 2, 3]


def test_set_vision_mode():
    mock = MockI2C([make_frame(bytes([0x01]))])
    driver = ESP32_P4C5_CAM(mock)
    assert driver.set_vision_mode(VISION_MODE_COLOR, [1, 2]) is True

    with pytest.raises(ESP32_P4C5_CAM_Error):
        driver.set_vision_mode(VISION_MODE_LINE, [1, 2])  # Line mode requires exactly 1 target


def test_color_result():
    # 1 block: index=2, x=100, y=120, w=50, h=40
    blk = bytes([0x02]) + struct.pack('<HHHH', 100, 120, 50, 40)
    payload = bytes([1]) + blk
    mock = MockI2C([make_frame(payload)])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_color_result()
    assert res['count'] == 1
    assert res['blocks'][0]['index'] == 2
    assert res['blocks'][0]['x'] == 100
    assert res['blocks'][0]['y'] == 120
    assert res['blocks'][0]['width'] == 50
    assert res['blocks'][0]['height'] == 40


def test_line_result():
    # detected = 1, start(10,20), end(100,200), angle=45.0, offset=-5.5
    data = bytes([0x01]) + struct.pack('<HHHHff', 10, 20, 100, 200, 45.0, -5.5)
    mock = MockI2C([make_frame(data)])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_line_result()
    assert res['detected'] is True
    assert res['start_x'] == 10
    assert res['start_y'] == 20
    assert res['end_x'] == 100
    assert res['end_y'] == 200
    assert abs(res['angle_deg'] - 45.0) < 1e-4
    assert abs(res['offset'] - (-5.5)) < 1e-4


def test_hand_gesture_result():
    # 1 gesture: ID=3 (OK gesture), bbox(50, 60, 80, 90)
    payload = bytes([1, 3]) + struct.pack('<HHHH', 50, 60, 80, 90)
    mock = MockI2C([make_frame(payload)])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_hand_gesture_result()
    assert res['total_count'] == 1
    assert res['parsed_count'] == 1
    assert res['items'][0]['gesture'] == 3
    assert res['items'][0]['x'] == 50
    assert res['items'][0]['width'] == 80


def test_target_detections():
    # Cat detection
    payload = bytes([1, 0]) + struct.pack('<HHHH', 10, 20, 30, 40)
    mock = MockI2C([make_frame(payload)])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_cat_result()
    assert res['total_count'] == 1
    assert res['items'][0]['x'] == 10

    # YOLO11n detection with valid class ID
    mock = MockI2C([make_frame(bytes([1, 15]) + struct.pack('<HHHH', 10, 20, 30, 40))])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_yolo11n_result()
    assert res['items'][0]['index'] == 15

    # YOLO11n detection with class ID > 79
    mock = MockI2C([make_frame(bytes([1, 80]) + struct.pack('<HHHH', 10, 20, 30, 40))])
    driver = ESP32_P4C5_CAM(mock)
    with pytest.raises(ESP32_P4C5_CAM_Error) as exc:
        driver.get_yolo11n_result()
    assert exc.value.code == 'invalid_response'


def test_pose_result():
    # 1 pose, bbox(10, 20, 100, 200), 2 keypoints: (idx=0, x=15, y=25), (idx=1, x=20, y=30)
    # Header format: index (1B), x, y, w, h (8B), kp_count (1B) -> 10 bytes per pose record
    record_header = bytes([0]) + struct.pack('<HHHH', 10, 20, 100, 200) + bytes([2])
    kp0 = bytes([0]) + struct.pack('<HH', 15, 25)
    kp1 = bytes([1]) + struct.pack('<HH', 20, 30)
    payload = bytes([1]) + record_header + kp0 + kp1
    mock = MockI2C([make_frame(payload)])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_pose_result()
    assert res['total_count'] == 1
    assert res['items'][0]['keypoint_count'] == 2
    assert res['items'][0]['keypoints'][0]['x'] == 15
    assert res['items'][0]['keypoints'][1]['y'] == 30


def test_apriltag_result():
    # 1 AprilTag: id=42, bbox(5, 5, 20, 20), rot=1.5, x_t=0.1, y_t=0.2, z_t=0.5, x_r=0.0, y_r=0.1, z_r=0.2
    payload_tag = struct.pack('<HHHHHfffffff', 42, 5, 5, 20, 20, 1.5, 0.1, 0.2, 0.5, 0.0, 0.1, 0.2)
    full_payload = bytes([1]) + payload_tag  # total = 39 bytes -> needs 2 fragments
    f1 = make_frame(full_payload[:20], fragment_id=1, total_fragments=2)
    f2 = make_frame(full_payload[20:], fragment_id=2, total_fragments=2)

    mock = MockI2C([f1, f2])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_apriltag_result()
    assert res['total_count'] == 1
    assert res['items'][0]['id'] == 42
    assert abs(res['items'][0]['rotation'] - 1.5) < 1e-4
    assert abs(res['items'][0]['z_translation'] - 0.5) < 1e-4


def test_qrcode_result():
    text = "https://hanerqaq.com"
    encoded_text = text.encode('utf-8')
    record_header = bytes([0]) + struct.pack('<HHHHH', 10, 20, 50, 50, len(encoded_text))
    full_payload = bytes([1]) + record_header + encoded_text
    # Total payload: 1 + 11 + 20 = 32 bytes -> fits in 2 fragments
    f1 = make_frame(full_payload[:20], fragment_id=1, total_fragments=2)
    f2 = make_frame(full_payload[20:], fragment_id=2, total_fragments=2)

    mock = MockI2C([f1, f2])
    driver = ESP32_P4C5_CAM(mock)
    res = driver.get_qrcode_result()
    assert res['total_count'] == 1
    assert res['items'][0]['text'] == text
    assert res['items'][0]['x'] == 10
    assert res['items'][0]['width'] == 50
