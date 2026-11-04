# recordPan

基于 ESP32-S3 的双麦克风录音笔，配一个原生 iOS App 做语音转文字。

这是一个 **AI 设计挑战赛**作品：方案设计、固件、App、文档全部由 AI（Claude Code）完成，人负责采购元件、接线、按键测试和反馈。

## 现在能做什么

- 🎙️ **双麦录音**：两个 INMP441 数字麦克风共用一条 I2S 总线，经过 80 Hz 高通滤波（去直流和低频轰鸣）、数字增益和软限幅后，取两个麦的平均值存成单声道。
- 📦 **Opus 压缩**：录音存为 Ogg Opus，24 kbps，每小时约 10.8 MB。板载 12 MB Flash 能录将近 1 小时。也可以切换成 WAV。
- 🔖 **标记键**：录音时按一下就打一个时间标记，结束后和时长一起写进同名的 `.json` 文件，供 App 使用。
- 🖥️ **OLED 状态屏**：显示录音计时、剩余可录时间、音量条，还有一只会动的 Clawd。待机时会晃动、眨眼，久了会睡着；有人说话时会跟着蹦跳；打标记时会举手欢呼。
- 📶 **蓝牙**：手机可以通过 BLE 开始/停止录音、打标记、查看状态和文件列表，录音时实时收到 Opus 音频流（协议见 [docs/BLE.md](docs/BLE.md)）。
- 📱 **iPhone 实时转写**：原生 iOS App 通过蓝牙接收音频，用苹果自带的语音识别边录边出中文（见 [ios/](ios/)）。
- 🔔 **提示**：RGB LED 显示状态，有源蜂鸣器在开始和停止时提示。提示音不会被录进去。

## 硬件

| 元件 | 数量 | 用途 |
|---|---|---|
| ESP32-S3 N16R8 开发板（16 MB Flash / 8 MB PSRAM） | 1 | 主控 |
| INMP441 I2S 数字麦克风 | 2 | 录音（L/R 分别接 GND / 3V3，组成左右声道） |
| 0.96" SSD1306 OLED（I2C，128×64） | 1 | 状态显示 |
| 有源蜂鸣器模块（高电平触发） | 1 | 提示音 |
| 按键 | 1 | 标记键（录音键用板载 BOOT 键） |
| 面包板、杜邦线 | — | 原型搭建 |

原型阶段不打 PCB，外壳造型留到最后阶段设计。完整引脚表见 [docs/PLAN.md](docs/PLAN.md)，接线和烧录步骤见 [firmware/README.md](firmware/README.md)。

## 系统结构

```
2×INMP441 ─I2S─► 采集（16 kHz）─► 高通 / 增益 / 软限幅 ─► 双麦平均 ─► Opus 编码
                                                              ├─► Flash：REC_NNNN.opus + .json
                                                              └─► BLE 实时音频 ─► iPhone ─► 语音转文字（M3/M4）
iPhone ◄─BLE─ 控制 / 状态 / 文件列表
iPhone ◄─Wi-Fi─ 批量下载录音（M5）
```

## 进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | I2S 采集、写 WAV、按键、LED | ✅ 完成 |
| M2 | Opus 编码、OLED、标记键、蜂鸣器、元数据文件 | ✅ 完成 |
| M3 | 固件 BLE：控制、状态、文件列表、实时音频流 | ✅ 完成 |
| M4 | iOS App：连接设备、控制录音、边录边转文字 | ✅ 完成 |
| M5 | Wi-Fi 批量同步、录音列表、播放、离线转写 | ⏳ 下一步 |
| M6 | 联调、外壳造型、演示视频与文档 | — |

## 目录

| 路径 | 内容 |
|---|---|
| [docs/PLAN.md](docs/PLAN.md) | 方案：选型、引脚分配、数据流、里程碑 |
| [firmware/](firmware/) | ESP-IDF v6.1 固件（C） |
| [tools/pull_recordings.ps1](tools/pull_recordings.ps1) | 从板子的 Flash 取出录音文件 |
| [tools/ble_test.py](tools/ble_test.py) | 电脑端蓝牙测试：控制录音、收实时音频 |
| [docs/BLE.md](docs/BLE.md) | 蓝牙协议 |
| [ios/](ios/) | iOS App（Swift / SwiftUI）：蓝牙控制、实时转写 |

## 快速开始

```powershell
C:\Espressif\v6.1\esp-idf\export.ps1
cd firmware
idf.py build
idf.py -p COM7 flash monitor
```

按 BOOT 键开始和停止录音，然后把录音取到 `recordings/`：

```powershell
.\tools\pull_recordings.ps1 -Port COM7
```

`.opus` 文件用 VLC、Chrome 或 ffplay 都能直接播放。

## 致谢

- OLED 上的 Clawd 按 [clawd-emotes-skill](https://github.com/xixicc186/clawd-emotes-skill) 中的方块结构绘制成像素画。
- Opus 编码使用乐鑫的 [esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec) 组件。
