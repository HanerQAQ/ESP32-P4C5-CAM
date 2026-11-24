# Image_transmission_function_AP Testing Guide

**English** | [简体中文](TEST_GUIDE_zh.md)

This document outlines the standard testing and factory validation procedures for the **ESP32-P4C5-CAM** AP mode wireless streaming firmware. Testing is performed over Wi-Fi without an onboard LCD.

---

## 1. Test Preparation

- Module under test, compatible camera sensor (GC2145), stable power supply (**5V ≥ 2A**), and Type-C cable.
- PC or smartphone with Wi-Fi support (**2.4 GHz Channel 1 by default** in `sdkconfig.defaults`; **5 GHz / Wi-Fi 6** supported when configured via `idf.py menuconfig` -> `Example Configuration` -> `Wi-Fi AP band`).
- A clean, uncreased, non-reflective `tag36h11` AprilTag printout (record its ID in advance).
- Plain white paper and colorful objects with text for optical quality inspection.
- An external I²C host controller with 3.3V logic levels (recommended bus speed `100 kHz`).

> [!WARNING]
> Always power down before modifying wiring. The I²C host and test board must share a common ground (`GND`). **Never connect 5V logic signals directly to the ESP32-P4 GPIO pins.**

| Feature | GPIO Pin / Configuration |
| :--- | :--- |
| **BOOT Test Button** | `GPIO 35` (Active Low) |
| **External I²C SCL / SDA** | `GPIO 47` / `GPIO 46` |
| **External I²C Slave** | I2C1 Slave, 7-bit address `0x54` |
| **Camera Sensor I²C SCL / SDA** | `GPIO 29` / `GPIO 28` (Isolated from external I²C) |
| **C5 SDIO Reset** | `GPIO 50` (Active High, 700 ms pulse) |

---

## 2. Build, Flash & Connect

```bash
cd firmware/Image_transmission_function_AP
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor   # Replace COMx with your serial port
```

1. After boot, connect to the SoftAP: `ESP32-P4C5-CAM-XXXXXX` (where `XXXXXX` matches the last 3 bytes of the MAC address printed in serial monitor). No password required.
2. By default (`sdkconfig.defaults`), the AP operates on **2.4 GHz (Channel 1)** for maximum compatibility. If switched to 5 GHz in `idf.py menuconfig`, it uses Channels 36/40/44/48 with Wi-Fi 6 enabled.
3. Stay connected even if your OS warns "No Internet Access". Open a web browser to `http://192.168.5.1`.
4. The page title displays `ESP32P4C5-Mini`, starting in "Streaming Mode" by default. Video streams on port `81`.

---

## 3. Streaming & Optical Quality Inspection

Under "Streaming Mode", verify:

1. Video updates continuously; moving an object in front of the lens shows smooth frame rate without freeze or tear.
2. Default resolution starts at **`640×480`** (center-cropped from the native `800×600` GC2145 capture) and can be switched to full **`800×600`** in Web Settings.
3. Aim at a plain white background under even illumination; check for dark spots, dust, vignetting, or uneven brightness.
4. Aim at colorful objects with text to check optical sharpness, color accuracy, and absence of color banding.
5. Click **Download JPEG** to verify the snapshot file; click **Download Raw** to verify non-empty RGB565 raw data (`614,400` bytes at `640×480`, or `960,000` bytes at `800×600`).
6. Adjust JPEG quality in the Web settings menu, save, and confirm stream recovers.

---

## 4. BOOT Button & Mode Switching

1. In "Streaming Mode", short-press the `GPIO 35` BOOT button. The Web page switches to "Test Mode".
2. Release and press again; the stream returns to "Streaming Mode".
3. Long-pressing should not repeatedly toggle states.
4. Verify reliable toggling without false triggers or double clicks.

---

## 5. AprilTag Detection Test

1. With the browser stream open, short-press BOOT to enter "Test Mode" (continuous inference mode).
2. Place the `tag36h11` target in the camera view with adequate lighting.
3. Verify that the web page overlays a green bounding box around the tag and displays the correct tag ID.
4. Move the tag slowly and confirm the bounding box tracks the movement.
5. Short-press BOOT to return to "Streaming Mode"; confirm the overlay disappears and normal video resumes.

---

## 6. External I²C Communication Test

This test verifies the communication header `GPIO 47 / 46` (not the camera I²C on GPIO 29/28).

1. With power off, wire Host SCL → `GPIO 47`, SDA → `GPIO 46`, GND → `GND`. Power up the system.
2. In browser, navigate to `http://192.168.5.1/api/tests`. The initial `i2c` status is `0` (waiting).
3. The host initiates a raw write transaction to 7-bit slave address `0x54`, transmitting exactly 14 ASCII bytes:
   ```text
   ESP32P4C5-MINI
   Hex: 45 53 50 33 32 50 34 43 35 2D 4D 49 4E 49
   ```
   Do not append register offsets, NUL, CR, or LF characters.

   *Linux host example (replace `BUS` with your I²C bus number):*
   ```bash
   i2ctransfer -y BUS w14@0x54 0x45 0x53 0x50 0x33 0x32 0x50 0x34 0x43 0x35 0x2d 0x4d 0x49 0x4e 0x49
   ```

4. Refresh `http://192.168.5.1/api/tests`. The `i2c` field updates to `1` (PASS).

---

## 7. Results API & Status Verification

Access `http://192.168.5.1/api/tests` for machine-readable test statuses:

| Key | Description |
| :--- | :--- |
| `camera` | `1` once at least one AprilTag has been successfully recognized |
| `boot` | `1` once a valid BOOT button press is detected |
| `i2c` | `1` once the exact 14-byte match string has been received |
| `all_passed` | `true` when all three items are `1` (PASS) |

Status values: `0` = Waiting, `1` = Passed, `2` = Pin unconfigured, `3` = Init failed.

---

## 8. Troubleshooting Matrix

| Symptom | Primary Checkpoints |
| :--- | :--- |
| **SoftAP SSID Not Found** | Confirm client supports 5 GHz Wi-Fi; check 5V 2A power supply; check C5/SDIO boot logs |
| **Web UI Opens but No Video** | Check port 81 access; ensure no other browser tabs are streaming; inspect camera init logs |
| **BOOT Button Does Not Toggle** | Verify pin GPIO 35; check pull-up resistor and active-low ground contact |
| **No AprilTag Bounding Box** | Confirm "Test Mode" is active; check tag family (`tag36h11`); improve ambient lighting |
| **I²C Test Fails (`i2c=0`)** | Check GPIO 47/46 wiring, common ground, 3.3V pull-ups, 7-bit address `0x54`, exact 14 ASCII bytes |
| **`i2c=3` in API** | Check serial monitor for `External I2C init:` errors (pin conflict or driver issue) |
| **`mempool_alloc failed`** | Reduce concurrent streaming clients; ensure stable power supply |

---

## 9. Factory Test Sheet

| Item | Result / Record |
| :--- | :--- |
| **Board S/N / SSID Suffix** | |
| **Test Date / Operator** | |
| **Firmware Version / Build Time** | |
| **Video Stream & Snapshot Downloads** | PASS / FAIL |
| **Optical Uniformity & Cleanliness** | PASS / FAIL |
| **BOOT Button Mode Toggling** | PASS / FAIL |
| **AprilTag Recognition (ID)** | Expected: _____ / Actual: _____ |
| **External I²C Communication** | PASS / FAIL |
| **API Test Status (`/api/tests`)** | `camera=` / `boot=` / `i2c=` |
| **Overall Verdict** | **PASS / FAIL / RETEST** |
