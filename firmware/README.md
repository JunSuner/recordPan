# recordPan 固件

ESP32-S3 N16R8 + INMP441 录音笔固件，基于 ESP-IDF v6.1。

## 功能

- 按**录音键**（板载 BOOT 键）开始录音，再按一次停止。录音保存为 `REC_0001.opus`、`REC_0002.opus`……：Ogg Opus，16 kHz 单声道，默认 24 kbps（每小时约 10.8 MB）。双麦时存的是两个麦的平均值。
- 录音时按**标记键**打一个标记，LED 闪一下蓝色，OLED 底部反白显示 `MARK n`。
- 停止时生成同名的 `REC_NNNN.json`，例如：
  ```json
  {"audio":"REC_0009.opus","sample_rate":16000,"channels":1,"duration_ms":28720,"marks_ms":[19780,21800]}
  ```
- 每 5 秒同步一次文件，录音中途断电也能保留之前的内容。
- 音频处理：80 Hz 高通滤波（去直流和低频轰鸣）+ 数字增益 + 软限幅。
- 蜂鸣器（可选）：开始录音响一声，停止响两声，出错长响。开始那声落在麦克风预热丢弃的 200 ms 内，停止那声在录音结束后，都不会录进去；标记键不响。
- LED 状态：暗绿 = 待机，红 = 录音中，红色闪烁 = 存储挂载失败（按录音键会重试）。
- OLED（可选，开机自动检测）：录音状态、剩余可录时间、计时、音量条、文件名和标记数。

## 蓝牙（M3）

开机后以 `recordPan-XXXX` 广播，手机或电脑连上后可以开始/停止录音、打标记、读取状态和文件列表，录音时还能订阅实时 Opus 音频。协议见 [docs/BLE.md](../docs/BLE.md)。

在电脑上测试（需要蓝牙和 `pip install bleak`）：

```powershell
python toolsle_test.py --seconds 12
```

## 录音格式

`idf.py menuconfig` → **recordPan board** → **Recording format**：

| 选项 | 说明 |
|---|---|
| **Opus**（默认） | `.opus`，单声道；码率和编码复杂度可在下面两项调整 |
| WAV | `.wav`，16 bit PCM，双麦时保留左右两个声道，适合检查麦克风 |

## 存储位置

`idf.py menuconfig` → **recordPan board** → **Recording storage**：

| 选项 | 说明 |
|---|---|
| **Onboard flash**（默认） | 板载 Flash 上 12 MB 的 FAT 分区，挂载在 `/flash`，不用接线；Opus 约能录 1 小时（WAV 单声道约 6 分钟）。第一次开机会自动格式化。 |
| microSD card | SD 卡模块，挂载在 `/sdcard`。 |

**关于 SD 卡供电**：常见的 6 针 SD 模块带 AMS1117 稳压，需要 5V 输入；但这款 ESP32-S3 板子的 5V 脚默认不输出电（板背面的 `IN-OUT` 焊盘未短接）。接 3V3 时卡能握手，但初始化会超时。解决办法：短接 `IN-OUT` 焊盘，或换用无稳压的 3.3V SD 模块。

## 接线

| 外设 | 引脚 | ESP32-S3 |
|---|---|---|
| INMP441 | SCK / WS / SD | GPIO4 / GPIO5 / GPIO6 |
| | L/R | GND |
| | VDD / GND | 3V3 / GND |
| 第二个 INMP441（双麦，默认开启） | SCK / WS / SD | 与第一个共用 GPIO4 / GPIO5 / GPIO6 |
| | L/R | **3V3**（右声道） |
| | VDD / GND | 3V3 / GND |
| 有源蜂鸣器（高电平触发） | I/O | GPIO15（模块 VCC 接 3V3，GND 接 GND） |
| OLED SSD1306 0.96" 128×64（I2C） | SDA / SCL（有的屏标 SCK） | GPIO41 / GPIO42 |
| | VCC / GND | 3V3 / GND |
| 录音键 | 板载 BOOT 键，无需接线 | GPIO0 |
| 标记键 | 一脚接 GPIO2，另一脚接 GND（或用杜邦线碰 GND） | GPIO2 |
| RGB LED | 板载 WS2812 | GPIO48（v1.1 板可能是 GPIO38） |
| microSD（选用时） | CS / MOSI / SCK / MISO | GPIO10 / GPIO11 / GPIO12 / GPIO13 |
| | VCC | 见上文“关于 SD 卡供电” |

引脚都可以在 `idf.py menuconfig` → **recordPan board** 中修改。SD 卡需格式化为 **FAT32**（大于 32 GB 的卡可在“磁盘管理”里建一个 32 GB 的分区再选 FAT32）。

## 编译与烧录

在 PowerShell 中：

```powershell
C:\Espressif\v6.1\esp-idf\export.ps1
cd C:\Coding\ESP32\recordPan\firmware
idf.py build
idf.py -p COM7 flash monitor  # COM7 换成设备管理器里看到的端口
```

- 用原生 USB 口时，如果板子上跑的是别的程序（显示为 `303a:4001` 之类），第一次烧录需手动进下载模式：按住 BOOT，按一下 RST，松开 BOOT。
- 通过 USB/IP 远程连接时，烧录后用 `--after watchdog-reset` 复位，否则板子可能停在下载模式：

```powershell
cd build
esptool --chip esp32s3 -p COM7 --before default-reset --after watchdog-reset write-flash "@flash_args"
```

## 取出录音

录音存在板载 Flash 时，用脚本把整个存储分区读出来并解出文件，放到项目的 `recordings/` 目录：

```powershell
.\tools\pull_recordings.ps1 -Port COM7
```

## 测试步骤

1. 按录音键，LED 变红，日志每秒打印 `REC 00:01`、`REC 00:02`……
2. 对着麦克风说话 10 秒左右，中间按一下标记键（LED 闪蓝）。
3. 再按录音键停止，日志打印 `Stopped /flash/REC_0001.wav: ... dropped 0 bytes, 0 write errors`。
4. 运行 `pull_recordings.ps1` 取出文件，播放 `REC_0001.opus`（VLC、Chrome、ffplay 都能放），查看 `REC_0001.json` 中的标记时间。

## 常见问题

| 现象 | 排查 |
|---|---|
| LED 不亮 | menuconfig 中把 LED GPIO 改为 38 |
| 日志 `No OLED on SDA=...` | 检查 OLED 的 SDA/SCL 是否接反、VCC 是否接 3V3 |
| OLED 显示上下颠倒 | menuconfig → OLED → 打开 Rotate 180 degrees |
| SD `Mount failed` | 日志里的 `MISO probe` 一行：CS=1 悬空、CS=0 为低 → 模块没电；两种都为高 → 有电但仍失败，多半是供电电压不足 |
| 录音全是静音 | 用 WAV 格式录一段，日志里 `Mic L/R raw range` 为常数的那个麦没有数据：检查它的供电、SCK/WS/SD 和 L/R 接线 |
| 声音太小或爆音 | 调整 menuconfig 中的 Digital gain（每级 6 dB，默认 3；超过 -6 dBFS 的部分会被软限幅压缩，不会硬削顶） |
| 日志中 `dropped` 不为 0 | 存储写入太慢；SD 卡换一张 Class 10 / U1 卡 |
| `Write failed (storage full?)` | 存储满了，取出录音后删除，或换用 SD 卡 |
