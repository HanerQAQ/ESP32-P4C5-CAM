# -*- coding: utf-8 -*-
"""
ESP32_P4C5_CAM Raspberry Pi Demo: Single-Line Tracking
Communicates with ESP32-P4C5-CAM over native Linux I2C (/dev/i2c-1).
"""

import os
import sys
import time

try:
    from esp32_p4c5_cam import RawI2C, ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR, VISION_MODE_LINE
except ImportError:
    _SDK_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..', 'python_sdk'))
    if _SDK_PATH not in sys.path:
        sys.path.insert(0, _SDK_PATH)
    from esp32_p4c5_cam import RawI2C, ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR, VISION_MODE_LINE

I2C_BUS = RawI2C.get_default_bus((1, 7, 8, 0))
I2C_ADDR = ESP32_P4C5_CAM_I2C_ADDR


def sleep_ms(ms):
    time.sleep(ms / 1000.0)


def print_line_result(result):
    print('line_detected=%d' % (1 if result['detected'] else 0))
    if not result['detected']:
        return
    print('  start=(%d,%d) end=(%d,%d) angle=%.2f offset=%.3f' % (
        result['start_x'], result['start_y'], result['end_x'], result['end_y'],
        result['angle_deg'], result['offset']))


def main():
    print('[ESP32_P4C5_CAM] Starting Single-Line Tracking demo on Raspberry Pi...')
    print('[ESP32_P4C5_CAM] Opening I2C bus %d at slave address 0x%02X...' % (I2C_BUS, I2C_ADDR))

    try:
        with RawI2C(I2C_BUS) as i2c:
            llm = ESP32_P4C5_CAM(i2c, I2C_ADDR)
            sleep_ms(1000)
            # Set single line track target color [1]
            LINE_TARGETS = [1]
            if not llm.set_vision_mode(VISION_MODE_LINE, LINE_TARGETS):
                print('[ESP32_P4C5_CAM] Error: Device rejected line tracking mode setting')
                return
            config = llm.get_color_config()
            print('[ESP32_P4C5_CAM] Line mode configured: mode=%d targets=%s' % (config['mode'], config['targets']))

            print('[ESP32_P4C5_CAM] Initialization complete. Entering read loop (Press Ctrl+C to stop)...')
            while True:
                try:
                    result = llm.get_line_result()
                    print_line_result(result)
                except Exception as e:
                    print('[ESP32_P4C5_CAM] Read failed:', e)
                sleep_ms(20)
    except KeyboardInterrupt:
        print('\n[Exit] User interrupted.')


if __name__ == '__main__':
    main()
