# who_web

Reusable web backhaul for the ESP32-P4C5-CAM ESP32-P4 + ESP32-C5 camera board.

## Features

- ESP-Hosted SoftAP on the on-module ESP32-C5 (default 5 GHz channel 36).
- MJPEG preview on port 81, served from the LCD/detection frame pipeline.
- Single-file local web UI on port 80 with browser-side detection overlays.
- `GET /api/detections` normalized JSON, `GET /api/frame.jpg` snapshot.
- JPEG encoding only while an MJPEG client is connected.
- No LCD, I²C or detector task ownership changes: the encoder only copies the
  latest RGB565 frame through `WhoFrameCapNode::cam_fb_snapshot()`.

## Usage

Add `components/who_web` to `EXTRA_COMPONENT_DIRS`, require `who_web` from the
`main` component, then:

```cpp
#include "who_web.hpp"
#include "who_web_detect_json.hpp"

// after the frame pipeline has been created
ESP_ERROR_CHECK(who::web::WhoWeb::get_instance().start(
    "Cat Detection", "cat", lcd_disp_frame_cap_node));

// from any detector callback
who::web::WhoWeb::get_instance().set_objects_json(
    who::web::json::detect_objects(result, {"cat"}));
```

Result objects must be a JSON array. The UI recognizes:

```json
[{"label":"cat","score":0.9,"box":[x,y,w,h],
  "keypoints":[[x,y]],"line":[x1,y1,x2,y2],"id":3,
  "pose":{"x":0,"y":0,"z":0},"text":"payload"}]
```

Helper headers:

- `who_web_detect_json.hpp`: boxes, keypoints, COCO labels, line results.
- `who_web_apriltag_json.hpp`: tag id, box, rotation and pose.
- `who_web_qrcode_json.hpp`: QR boxes and payload.

## Configuration

`idf.py menuconfig -> ESP32-P4C5-CAM web backhaul`:

| Option | Default |
| --- | --- |
| SSID prefix | `ESP32-P4C5-CAM` (MAC suffix appended) |
| Password | empty (open AP) |
| 5 GHz only | enabled |
| Channel | 36 (5 GHz) / 6 (2.4 GHz) |
| HTTP / stream port | 80 / 81 |
| JPEG quality | 60 |
| Maximum stream FPS | 10 |

The board needs the ESP-IDF v6.1 `esp_hosted` / `esp_wifi_remote` stack and the
SDIO reset wiring already present in every example's `sdkconfig.defaults`:

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=50
CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=50
CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=700
CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y
CONFIG_SLAVE_IDF_TARGET_ESP32C5=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

The C5 SDIO receive path needs contiguous internal DMA memory. On this board the
LVGL UI is changed to 320x80 partial refreshes (see
`components/who_peripherals/who_lcd/who_lvgl_lcd.cpp`) so the 150 KB internal
staging buffer no longer starves ESP-Hosted. Keep that change when porting the
component to another project.

## Notes

- The SoftAP is open by default, has no authentication, and serves only the local
  preview. Do not deploy it as-is on an untrusted network.
- Only one MJPEG client is served at a time by the HTTP server. Close extra tabs.
- The web thread runs at priority 2 on core 0. It encodes only when a stream client
  exists, but a very high `WHO_WEB_STREAM_FPS` + JPEG quality can still compete
  with camera/display work.
- Detection coordinates must be in the same 320×240 LCD coordinate system passed
  to `start()`. The examples use `FrameCapPPAResizeLCD` for that reason.
