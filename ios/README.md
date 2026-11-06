# recordPan iOS App

原生 SwiftUI App：通过蓝牙连接录音笔，控制录音，并把录音笔传来的实时音频转成文字。

## 功能

- 自动扫描并连接 `recordPan-XXXX`，断开后会自动重连。
- 录音 / 停止 / 标记按钮，和录音笔上的实体键效果一样，两边状态同步。
- 显示计时、音量、标记数和剩余可录时长；还有一只会跟着声音蹦的 Clawd。
- **实时转写**：BLE 收到的 Opus 包用 iOS 自带的 Opus 解码器解成 16 kHz PCM，再交给 `SFSpeechRecognizer`（zh-CN）识别。设备支持时只在本机识别（端侧），并自动加标点。为了不超过识别时长限制，长录音每 50 秒换一段识别任务。
- 设备上的录音列表，每次录音结束后自动刷新。
- **录音库**（M5）：通过录音笔的 Wi-Fi 热点下载新录音到手机（[docs/WIFI.md](../docs/WIFI.md)）。免费 Apple ID 不能让 App 自动连热点，所以 App 会显示热点名和密码，在"设置"里手动连一次后就自动开始下载，下载完后录音笔自动关掉 Wi-Fi。
- 播放：Ogg Opus 先解码成 WAV 缓存，可以拖动进度条，点标记跳到标记前 2 秒。
- 离线转写：已下载的录音按 50 秒一段交给语音识别，结果和录音存在一起；边录边出的文字在录音结束后也会自动存到对应的录音下。
- 删除手机上或录音笔上的录音。

## 代码

| 文件 | 作用 |
|---|---|
| `Protocol.swift` | BLE UUID、命令，以及状态、文件、音频数据的解析（对应 [docs/BLE.md](../docs/BLE.md)） |
| `BLEManager.swift` | CoreBluetooth：扫描、连接、订阅、发送命令 |
| `OpusDecoder.swift` | 用 `AVAudioConverter` 解码 Opus，丢失的帧补静音 |
| `Transcriber.swift` | `SFSpeechRecognizer` 实时识别，分段处理 |
| `AppModel.swift` | 串起音频流：BLE → 解码 → 识别，跟随录音状态启停，把实时转写存到对应录音 |
| `RecordingStore.swift` | 手机上的录音库，以及经 Wi-Fi 从录音笔同步 |
| `OggOpus.swift` | 解析 Ogg Opus 文件、解码成 PCM、写 WAV |
| `Player.swift` | 播放与跳转 |
| `LibraryView.swift` / `RecordingDetailView.swift` | 录音库和录音详情界面 |
| `ContentView.swift` / `ClawdView.swift` | 界面 |

工程使用 Xcode 的文件夹同步（folder-synced group）：往 `RecordPan/` 里加 Swift 文件不需要改工程文件。

## 编译和安装

需要 Xcode 27 和 iOS 26 以上。用 Xcode 打开 `RecordPan.xcodeproj`，选择 iPhone，点运行即可。

工程里填的签名团队是项目作者的个人团队（`DEVELOPMENT_TEAM`），换成你自己的 Apple ID 团队就能编译。用免费账号签名时要注意：一台手机最多只能同时装 3 个这样签名的 App，而且 7 天后需要重新安装。

也可以在 Mac 的"终端"里一条命令完成编译、签名、安装和启动（脚本里写的是作者 iPhone 的设备 ID）：

```bash
./build_install.sh
```
