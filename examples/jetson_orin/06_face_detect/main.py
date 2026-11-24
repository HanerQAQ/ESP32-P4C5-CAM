# -*- coding: utf-8 -*-
"""
ESP32_P4C5_CAM Jetson Orin Demo: Human Face Detection
Communicates with ESP32-P4C5-CAM over native Linux I2C (/dev/i2c-7).
"""

import os
import sys
import time

try:
    from esp32_p4c5_cam import RawI2C, ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR
except ImportError:
    _SDK_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..', 'python_sdk'))
    if _SDK_PATH not in sys.path:
        sys.path.insert(0, _SDK_PATH)
    from esp32_p4c5_cam import RawI2C, ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR

I2C_BUS = RawI2C.get_default_bus((7, 1, 8, 0))
I2C_ADDR = ESP32_P4C5_CAM_I2C_ADDR


def sleep_ms(ms):
    time.sleep(ms / 1000.0)


def print_face_result(result):
    print('face_total=%d parsed=%d' % (result['total_count'], result['parsed_count']))
    for item in result['items']:
        print('  index=%d x=%d y=%d w=%d h=%d' % (
            item['index'], item['x'], item['y'], item['width'], item['height']))


def main():
    print('[ESP32_P4C5_CAM] Starting Human Face Detection demo on Jetson Orin...')
    print('[ESP32_P4C5_CAM] Opening I2C bus %d at slave address 0x%02X...' % (I2C_BUS, I2C_ADDR))

    try:
        with RawI2C(I2C_BUS) as i2c:
            llm = ESP32_P4C5_CAM(i2c, I2C_ADDR)
            sleep_ms(1000)

            print('[ESP32_P4C5_CAM] Initialization complete. Entering read loop (Press Ctrl+C to stop)...')
            while True:
                try:
                    result = llm.get_face_result()
                    print_face_result(result)
                except Exception as e:
                    print('[ESP32_P4C5_CAM] Read failed:', e)
                sleep_ms(20)
    except KeyboardInterrupt:
        print('\n[Exit] User interrupted.')


if __name__ == '__main__':
    main()
