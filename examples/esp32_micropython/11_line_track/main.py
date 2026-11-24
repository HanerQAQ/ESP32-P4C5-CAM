# -*- coding: utf-8 -*-
"""
ESP32_P4C5_CAM ESP32 MicroPython Demo: Single-Line Tracking
Communicates with ESP32-P4C5-CAM over hardware I2C (Fast Mode 400kHz).
"""

import sys
from machine import I2C, Pin
from time import sleep_ms

try:
    from ESP32_P4C5_CAM import ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR, VISION_MODE_LINE
except ImportError:
    if '../lib' not in sys.path:
        sys.path.append('../lib')
    if '/lib' not in sys.path:
        sys.path.append('/lib')
    from ESP32_P4C5_CAM import ESP32_P4C5_CAM, ESP32_P4C5_CAM_Error, ESP32_P4C5_CAM_I2C_ADDR, VISION_MODE_LINE

# I2C 硬件参数：设备地址、数据引脚、时钟引脚、通信速率
I2C_ADDR = ESP32_P4C5_CAM_I2C_ADDR
# Standard ESP32 DevKit default pins: SDA=21, SCL=22 (adjust SDA/SCL if using custom expansion boards)
I2C_SDA = 21
I2C_SCL = 22
I2C_FREQ_HZ = 400000


def print_line_result(result):
    print('line_detected=%d' % (1 if result['detected'] else 0))
    if not result['detected']:
        return
    print('  start=(%d,%d) end=(%d,%d) angle=%.2f offset=%.3f' % (
        result['start_x'], result['start_y'], result['end_x'], result['end_y'],
        result['angle_deg'], result['offset']))


def main():
    print('[ESP32_P4C5_CAM] Starting Single-Line Tracking demo on ESP32 MicroPython...')
    print('[ESP32_P4C5_CAM] Initializing I2C (SDA=%d, SCL=%d, %dHz)...' % (I2C_SDA, I2C_SCL, I2C_FREQ_HZ))
    i2c = I2C(0, scl=Pin(I2C_SCL), sda=Pin(I2C_SDA), freq=I2C_FREQ_HZ)
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
    try:
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
