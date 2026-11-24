

# 串口通信协议

[English](uart_protocol_en.md) | **简体中文**

## 1. 基本约定

- 上位机发送json文本，设备返回的数据需要经过协议帧封装
- 多字节整数均采用小端序。
- 帧头固定为 `AA 55`。
- `DATA` 区第 1 个字节固定为功能号 `FUNC`。
- `DATA` 区从第 2 个字节开始为功能数据 `PAYLOAD`。
- 校验位 `CHECKSUM` 对整个 `DATA` 区做逐字节异或校验，即包含 `FUNC` 和 `PAYLOAD`。
- 默认不进行分片，单帧数据使用 `FRAGMENT_ID = 1`、`TOTAL_FRAGMENTS = 1`。
- 所有功能要遵循模块化设计

```text
#校验算法
def calculate_checksum(data):
  checksum = 0
  for byte in data:
    checksum ^= byte
  return checksum & 0xFF
```



## 2. 帧结构

| 字段            |      长度 | 说明                                                         |
| --------------- | --------: | ------------------------------------------------------------ |
| HEADER_0        |         1 | 固定帧头 `0xAA`                                              |
| HEADER_1        |         1 | 固定帧头 `0x55`                                              |
| LEN             |         2 | `DATA` 区长度，包含 `FUNC`，不包含帧头、长度字段、分片字段和校验位 |
| FRAGMENT_ID     |         2 | 当前分片序号，从 `1` 开始                                    |
| TOTAL_FRAGMENTS |         2 | 总分片数                                                     |
| FUNC            |         1 | 功能号，属于 `DATA` 区第 1 字节                              |
| PAYLOAD         | `LEN - 1` | 功能数据                                                     |
| CHECKSUM        |         1 | 对 `DATA` 区执行异或校验                                     |



## 3. 功能号

| 功能号 | 名称     | PAYLOAD 格式        |
| -----: | -------- | ------------------- |
| `0x00` | 图像捕获 | JPEG 原始二进制分片 |
| `0x01` | 读取设置 | UTF-8 JSON          |
| `0x02` | 保存设置 | UTF-8 JSON          |



## 4. JSON 命令格式

除图像数据返回外，`PAYLOAD` 使用 UTF-8 JSON：

```json
{
  "command": "command_name",
  "params": {}
}
```



## 5. 图像捕获

### 5.1 上位机发送捕获请求

- 上位机默认图像尺寸为 `640 x 480`，图像编码固定为 JPEG。

- 发送以下指令后准备接收ESP32的图像数据，上位机收到 `1..TOTAL_FRAGMENTS` 的全部图像数据分片后，按分片序号拼接所有 `PAYLOAD`，得到完整 JPEG 字节流并显示到“当前画面”区域。

```json
{
  "command": "vision_setting",
  "params": "get_img_frame"
}
```

### 5.2 ESP32 返回图像数据

- 图像数据返回帧的 `FUNC` 固定为 `0x00`，`PAYLOAD` 直接放 JPEG 文件原始二进制字节。
- 整一帧最大1024个字节

| 字段            | 值                             |
| --------------- | ------------------------------ |
| FUNC            | `0x00`                         |
| FRAGMENT_ID     | `1` 到 `TOTAL_FRAGMENTS`       |
| TOTAL_FRAGMENTS | 图像数据总分片数               |
| PAYLOAD         | 当前分片的 JPEG 原始二进制数据 |



## 6. 读取设置

### 6.1 上位机读取设置指令

颜色配置不发送名称，使用 `index` 表示对应颜色，取值为 `1` 到 `5`。

```json
{
  "command": "vision_setting",
  "params": "get_color_config"
}
```

### 6.2 ESP32 返回颜色阈值参数

- 颜色配置不发送名称，使用 `index` 表示对应颜色，取值为 `1` 到 `5`。
- 设备中有多少个颜色的数据就返回多少
- 返回的参数中 `box_color` 代表检测框的颜色，`index` 表示颜色序号。
- 读取的是NVS里保存的数据

| 字段            | 值                              |
| --------------- | ------------------------------- |
| FUNC            | `0x01`                          |
| FRAGMENT_ID     | `1` 到 `TOTAL_FRAGMENTS`        |
| TOTAL_FRAGMENTS | JSON 数据总分片数，单帧时为 `1` |

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





## 7. 保存设置

### 7.1 上位机发送保存设置

- 颜色配置不发送名称，使用 `index` 表示对应颜色，取值为 `1` 到 `5`。
- 发送的是5个颜色卡的HSV数据
- 返回的参数中 `box_color`代表检测框的颜色，`name`、`hsv_min`、`hsv_max`均为`register_color`的参数， `box_color`的数值对应的识别颜色

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



`target` 为检测目标序号列表。默认颜色序号如下：

| 颜色     | 序号 |
| -------- | ---: |
| `color1` |  `1` |
| `color2` |  `2` |
| `color3` |  `3` |
| `color4` |  `4` |
| `color5` |  `5` |



### 7.2 ESP32 返回设置结果

- 如果收到上位机保存设置的数据，则把数据保存到esp32的NVS里，保存之前要对各参数的取值合法性进行判定
- nvs保存完后要再读出来，如果读出来的数据和和接受到的参数一致即为校验成功

| 字段            | 值     |
| --------------- | ------ |
| FUNC            | `0x02` |
| FRAGMENT_ID     | `1`    |
| TOTAL_FRAGMENTS | `1`    |

```json
{
  "command": "vision_setting",
  "params": {
    "is_save": true
  }
}
```

`is_save` 为设备运行模式，说明如下：

| is_save | 说明     |
| ------- | -------- |
| `true`  | 保存成功 |
| `false` | 保存失败 |



## 8. 分片规则

- 默认使用单帧数据：`FRAGMENT_ID = 1`，`TOTAL_FRAGMENTS = 1`。
- 一帧默认最多1024字节。
- 多帧数据：`FRAGMENT_ID` 从 `1` 开始递增，直到 `TOTAL_FRAGMENTS`。
- 同一次业务返回的所有分片必须使用相同的 `FUNC` 和 `TOTAL_FRAGMENTS`。
- JSON 分片时，上位机或 ESP32 先按分片顺序拼接 `PAYLOAD`，再按 UTF-8 JSON 解析。
- JPEG 图像分片时，上位机按分片顺序拼接 `PAYLOAD`，得到完整 JPEG 字节流。



## 9. 异常处理

- 帧头不是 `AA 55`：丢弃无效字节并重新同步。
- `LEN < 1`：无功能号，丢弃当前帧。
- 校验位不匹配：丢弃当前帧。
- 分片编号不在 `1..TOTAL_FRAGMENTS` 范围内：丢弃当前帧。
- 同一次业务的 `FUNC` 或 `TOTAL_FRAGMENTS` 不一致：丢弃当前业务缓存。
- JSON 解析失败：上位机提示协议解析错误。
- 图像不是 JPEG 或 JPEG 解码失败：上位机提示图像解析错误。
