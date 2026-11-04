# recordPan 方案（已定稿 v1）

ESP32-S3 录音笔 + 原生 iOS App 语音转文字。本项目由 AI（Claude Code）主导完成设计与编码，用于 AI 设计挑战赛。

## 已确定的选型

| 项 | 选择 |
|---|---|
| 主控 | ESP32-S3 N16R8 开发板（16MB Flash / 8MB Octal PSRAM） |
| 麦克风 | 2 × INMP441（I2S，共用总线，L/R 分别接 GND / 3V3） |
| 存储 | 目前用板载 Flash（12 MB FAT 分区）；microSD 待解决 5V 供电后启用 |
| 交互 | 录音键 + 标记键、板载 RGB LED、有源蜂鸣器、0.96" SSD1306 OLED（I2C） |
| 供电 | 暂用 USB 供电；电池与电量检测后续再加（预留 ADC 引脚） |
| 传输 | BLE 为主（控制 + 实时音频流），Wi-Fi 批量下载录音文件 |
| 实时转写 | 要：录音时音频经 BLE 推到手机实时识别 |
| App | 原生 iOS（Swift / SwiftUI），在 Mac 上用 Xcode 编译 |
| 语音转文字 | Apple Speech 框架（SFSpeechRecognizer，zh-CN，优先端侧识别） |

## 接线（引脚分配）

避开的引脚：GPIO26–32（Flash）、GPIO35–37（Octal PSRAM）、GPIO0/3/45/46（启动配置）、GPIO19/20（USB）。

| 外设 | 信号 | ESP32-S3 GPIO |
|---|---|---|
| INMP441 | SCK（BCLK） | 4 |
| | WS（LRCLK） | 5 |
| | SD（数据） | 6 |
| | L/R | 麦 1 接 GND（左声道），麦 2 接 3V3（右声道） |
| | VDD / GND | 3V3 / GND |
| microSD | CS | 10 |
| | MOSI | 11 |
| | SCK | 12 |
| | MISO | 13 |
| | VCC | 模块带 AMS1117 稳压的接 5V，否则接 3V3 |
| OLED SSD1306 | SDA | 41 |
| | SCL（屏上标 SCK） | 42 |
| 录音键 | 板载 BOOT 键，无需接线 | 0 |
| 标记键 | 一端接 GPIO，另一端接 GND（内部上拉） | 2 |
| 有源蜂鸣器 | 高电平触发 | 15 |
| 板载 RGB LED | WS2812 | 48（部分 v1.1 板为 38） |
| 电池电压（预留） | 100k/100k 分压 | 7 |

## 数据流

```
2×INMP441 ─I2S─► 采集任务(16kHz/16bit 立体声) ─► 80Hz 高通/增益/软限幅 ─► 双麦平均 ─► Opus 编码(24kbps)
                                                    ├─► 存储 REC_*.opus + .json(时长/标记点)
                                                    └─► BLE 实时音频通知 ─► iOS 解码 ─► SFSpeechRecognizer
iOS ◄─BLE GATT─ 控制/状态/文件列表
iOS ◄─Wi-Fi(SoftAP + HTTP)─ 批量下载录音文件
```

## 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 ✅ | 固件：I2S 采集，写 WAV，按键开始/停止，LED 指示 | 录出能正常播放的 wav |
| M2 ✅ | 固件：Opus 编码、OLED 显示、标记键、蜂鸣器、元数据文件 | 完整离线录音笔 |
| M3 ✅ | 固件：BLE GATT（控制/状态/文件列表/实时流），协议见 [BLE.md](BLE.md) | 电脑端脚本能控制录音并收到无丢包的实时音频 |
| M4 ✅ | iOS：连接设备、控制录音、实时转写界面 | 边录边出字 |
| M5 | 固件 Wi-Fi SoftAP + HTTP；iOS 批量同步、录音列表、播放、离线转写 | 同步并转写历史录音 |
| M6 | 联调、外壳造型、演示视频与文档 | 参赛材料 |

## 开发环境

- 固件：ESP-IDF v6.1（本机已安装于 `C:\Espressif\v6.1\esp-idf`）
- iOS：Xcode 15+，iOS 17+，Swift / SwiftUI，CoreBluetooth、Speech、AVFoundation
