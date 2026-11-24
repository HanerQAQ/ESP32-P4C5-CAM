# Serial Communication Protocol (Color Calibration & Detection)

**English** | [简体中文](uart_protocol_zh.md)

---

## 1. General Principles

- The desktop host sends raw UTF-8 JSON text commands. The module responds with encapsulated binary protocol frames.
- Multi-byte integers use **Little-Endian** byte order.
- Fixed 2-byte frame header: `0xAA 0x55`.
- The first byte of the `DATA` field is the function code `FUNC`.
- The `PAYLOAD` starts from the 2nd byte of the `DATA` field (`offset = 1`).
- The `CHECKSUM` byte is computed by byte-by-byte XOR over the entire `DATA` field (`FUNC` + `PAYLOAD`).
- Single-frame responses use `FRAGMENT_ID = 1` and `TOTAL_FRAGMENTS = 1`.
- Max frame size is 1024 bytes.

```python
# Checksum calculation algorithm
def calculate_checksum(data: bytes) -> int:
  checksum = 0
  for byte in data:
    checksum ^= byte
  return checksum & 0xFF
```

---

## 2. Frame Structure

| Field | Length (Bytes) | Description |
| :--- | :---: | :--- |
| `HEADER_0` | 1 | Fixed frame sync byte `0xAA` |
| `HEADER_1` | 1 | Fixed frame sync byte `0x55` |
| `LEN` | 2 | `DATA` field length (`1 + len(PAYLOAD)`), Little-Endian |
| `FRAGMENT_ID` | 2 | Current fragment sequence number (starts from `1`), Little-Endian |
| `TOTAL_FRAGMENTS` | 2 | Total number of fragments for this transmission, Little-Endian |
| `FUNC` | 1 | Function code (first byte of `DATA`) |
| `PAYLOAD` | `LEN - 1` | Function payload data (raw JPEG binary or UTF-8 JSON) |
| `CHECKSUM` | 1 | XOR checksum computed over `FUNC` + `PAYLOAD` |

---

## 3. Function Codes

| Function Code | Name | Payload Format | Direction |
| :---: | :--- | :--- | :--- |
| `0x00` | Image Capture | Raw JPEG binary slice | Module -> Host |
| `0x01` | Read Settings | UTF-8 JSON | Module -> Host |
| `0x02` | Save Settings | UTF-8 JSON | Module -> Host |

---

## 4. JSON Command Format

Except for raw JPEG image returns, all commands and JSON payloads use UTF-8:

```json
{
  "command": "command_name",
  "params": {}
}
```

---

## 5. Image Capture

### 5.1 Host Request Frame

The host requests a live camera frame (default `640 x 480`, JPEG compressed):

```json
{
  "command": "vision_setting",
  "params": "get_img_frame"
}
```

### 5.2 Module Response (JPEG Data)

- Response frame `FUNC` is fixed to `0x00`.
- The `PAYLOAD` contains raw JPEG binary bytes.
- The host reassembles fragments `1..TOTAL_FRAGMENTS` in sequence into a complete JPEG file and renders it in the preview workspace.

| Field | Value |
| :--- | :--- |
| `FUNC` | `0x00` |
| `FRAGMENT_ID` | `1` to `TOTAL_FRAGMENTS` |
| `TOTAL_FRAGMENTS` | Total image slice count |
| `PAYLOAD` | JPEG binary slice data (up to 1015 bytes per frame) |

---

## 6. Read Settings

### 6.1 Host Read Command

Color cards do not pass names; they are identified by `index` (`1` to `5`):

```json
{
  "command": "vision_setting",
  "params": "get_color_config"
}
```

### 6.2 Module Parameter Response

Returns HSV thresholds stored in module NVS:

| Field | Value |
| :--- | :--- |
| `FUNC` | `0x01` |
| `FRAGMENT_ID` | `1` |
| `TOTAL_FRAGMENTS` | `1` |

```json
{
  "command": "vision_setting",
  "params": [
    {
      "index": 1,
      "hsv_min": [170, 100, 100],
      "hsv_max": [179, 255, 255]
    },
    {
      "index": 2,
      "hsv_min": [20, 100, 100],
      "hsv_max": [35, 255, 255]
    },
    {
      "index": 3,
      "hsv_min": [35, 100, 100],
      "hsv_max": [85, 255, 255]
    },
    {
      "index": 4,
      "hsv_min": [100, 100, 100],
      "hsv_max": [130, 255, 255]
    },
    {
      "index": 5,
      "hsv_min": [135, 100, 100],
      "hsv_max": [165, 255, 255]
    }
  ]
}
```

---

## 7. Save Settings

### 7.1 Host Save Command

Sends calibrated HSV ranges and UI bounding-box display colors to the module:

```json
{
  "command": "vision_setting",
  "params": {
    "color_config": [
      {
        "index": 1,
        "hsv_min": [170, 100, 100],
        "hsv_max": [179, 255, 255],
        "box_color": [255, 0, 0]
      }
    ]
  }
}
```

Default color indices:

| Color Index | Label |
| :---: | :--- |
| `1` | `color1` (Red) |
| `2` | `color2` (Green) |
| `3` | `color3` (Blue) |
| `4` | `color4` (Yellow) |
| `5` | `color5` (Black/Line) |

### 7.2 Module Save Confirmation

The module validates the HSV values, commits them to NVS, reads them back for verification, and returns the result:

| Field | Value |
| :--- | :--- |
| `FUNC` | `0x02` |
| `FRAGMENT_ID` | `1` |
| `TOTAL_FRAGMENTS` | `1` |

```json
{
  "command": "vision_setting",
  "params": {
    "is_save": true
  }
}
```

---

## 8. Fragmentation & Reassembly

- **Single Frame**: `FRAGMENT_ID = 1`, `TOTAL_FRAGMENTS = 1`.
- **Multi-Frame**: `FRAGMENT_ID` increments monotonically from `1` to `TOTAL_FRAGMENTS`.
- All slices belonging to the same transaction must share identical `FUNC` and `TOTAL_FRAGMENTS`.
- For JSON fragments, concatenate `PAYLOAD` bytes in order before parsing as UTF-8 JSON.
- For JPEG slices, concatenate `PAYLOAD` bytes to reconstruct the complete image.

---

## 9. Error Handling

- **Header Desynchronization**: If bytes do not match `0xAA 0x55`, drop bytes until sync pattern is found.
- **Length Error (`LEN < 1`)**: Discard malformed frame.
- **Checksum Mismatch**: Discard frame and request retry.
- **Out-of-Bounds Fragment**: Discard frame if `FRAGMENT_ID > TOTAL_FRAGMENTS`.
- **JSON Parse Failure**: Host displays a protocol parsing error hint.
