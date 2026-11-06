# recordPan Wi-Fi 同步

录音笔的 Wi-Fi 平时是关着的。手机通过蓝牙发送 `WIFI_ON`（`0x05`）命令打开它，用完后发送 `WIFI_OFF`（`0x06`）关闭。如果 10 分钟内没有收到任何 HTTP 请求，录音笔也会自动关掉 Wi-Fi。Wi-Fi 打开时，蓝牙状态的 flags 字段 bit 0 为 1，OLED 顶部会显示 `WiFi`。

## 热点

| 项 | 值 |
|---|---|
| SSID | `recordPan-XXXX`（和蓝牙名相同） |
| 加密 | WPA2-PSK |
| 密码 | 每次开机随机生成 10 位，只通过蓝牙告诉手机 |
| 地址 | `192.168.4.1`，端口 80 |

读取蓝牙的 Wi-Fi 特征值（`6a2e0006-…`），得到三行 UTF-8 文本：SSID、密码、IP。

iOS 上用 `NEHotspotConfiguration` 自动连热点需要 Hotspot Configuration 权限，免费的 Apple ID 用不了。所以 App 会显示热点名和密码，让用户在"设置"里手动连接，然后每 3 秒检测一次文件服务，连通就开始下载。

## HTTP 接口

### `GET /api/files`

列出所有录音（`.opus` / `.wav`），并把每个录音的 `.json` 元数据原样嵌进来。正在录的文件不会出现在列表里。

```json
{
  "recording": null,
  "files": [
    {"name": "REC_0009.opus", "size": 84892,
     "meta": {"audio": "REC_0009.opus", "sample_rate": 16000, "channels": 1,
              "duration_ms": 28720, "marks_ms": [19780, 21800]}},
    {"name": "REC_0001.wav", "size": 149804, "meta": null}
  ]
}
```

- `recording`：录音进行中时，是当前录音的文件名（不含扩展名），否则为 `null`。
- `meta`：没有元数据文件的录音（M1 时期的老录音）为 `null`。

### `GET /files/<name>`

下载文件，按分块传输（chunked）返回。`Content-Type` 为 `audio/ogg`、`audio/wav` 或 `application/json`。

### `DELETE /files/<name>`

删除音频文件和同名的 `.json`。

| 返回码 | 含义 |
|---|---|
| `204` | 删除成功 |
| `404` | 文件不存在或文件名不合法 |
| `409` | 这个文件正在录音中，不能删 |

文件名只接受 `REC_` 开头、只含字母、数字、`_` 和 `.` 的名字，防止路径穿越。删除也可以不开 Wi-Fi，直接走蓝牙的 `DELETE`（`0x07` + 文件名）命令。
