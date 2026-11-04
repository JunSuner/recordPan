# recordPan iOS App

原生 SwiftUI App：通过蓝牙连接录音笔，控制录音，并把录音笔传来的实时音频转成文字。

## 功能（M4）

- 自动扫描并连接 `recordPan-XXXX`，断开后会自动重连。
- 录音 / 停止 / 标记按钮，和录音笔上的实体键效果一样，两边状态同步。
- 显示计时、音量、标记数和剩余可录时长；还有一只会跟着声音蹦的 Clawd。
- **实时转写**：BLE 收到的 Opus 包用 iOS 自带的 Opus 解码器解成 16 kHz PCM，再交给 `SFSpeechRecognizer`（zh-CN）识别。设备支持时只在本机识别（端侧），并自动加标点。为了不超过识别时长限制，长录音每 50 秒换一段识别任务。
- 设备上的录音列表，每次录音结束后自动刷新。

## 代码

| 文件 | 作用 |
|---|---|
| `Protocol.swift` | BLE UUID、命令，以及状态、文件、音频数据的解析（对应 [docs/BLE.md](../docs/BLE.md)） |
| `BLEManager.swift` | CoreBluetooth：扫描、连接、订阅、发送命令 |
| `OpusDecoder.swift` | 用 `AVAudioConverter` 解码 Opus，丢失的帧补静音 |
| `Transcriber.swift` | `SFSpeechRecognizer` 实时识别，分段处理 |
| `AppModel.swift` | 串起音频流：BLE → 解码 → 识别，并跟随录音状态启停 |
| `ContentView.swift` / `ClawdView.swift` | 界面 |

工程使用 Xcode 的文件夹同步（folder-synced group）：往 `RecordPan/` 里加 Swift 文件不需要改工程文件。

## 编译和安装

需要 Xcode 27 和 iOS 26 以上。用 Xcode 打开 `RecordPan.xcodeproj`，选择 iPhone，点运行即可。

工程里填的签名团队是项目作者的个人团队（`DEVELOPMENT_TEAM`），换成你自己的 Apple ID 团队就能编译。用免费账号签名时要注意：一台手机最多只能同时装 3 个这样签名的 App，而且 7 天后需要重新安装。

也可以在 Mac 的"终端"里一条命令完成编译、签名、安装和启动（脚本里写的是作者 iPhone 的设备 ID）：

```bash
./build_install.sh
```
