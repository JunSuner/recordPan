import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    private var ble: BLEManager { model.ble }
    private var recording: Bool { ble.status?.state == .recording }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    deviceCard
                    transcriptCard
                    filesCard
                }
                .padding()
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("recordPan")
        }
    }

    // MARK: Device

    private var linkText: String {
        switch ble.link {
        case .bluetoothOff: "蓝牙未开启"
        case .scanning: "正在寻找录音笔…"
        case .connecting: "正在连接…"
        case .connected(let name): name
        }
    }

    private var level: Double {
        guard recording, let db = ble.status?.peakDb else { return 0 }
        return min(max(Double(db + 60) / 60, 0), 1)
    }

    private var deviceCard: some View {
        VStack(spacing: 12) {
            HStack {
                Circle()
                    .fill(ble.isConnected ? Color.green : Color.orange)
                    .frame(width: 8, height: 8)
                Text(linkText).font(.subheadline).foregroundStyle(.secondary)
                Spacer()
                if let s = ble.status {
                    Text(s.state == .noStorage ? "存储不可用" : "剩余约 \(s.remainingMinutes) 分钟")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            ClawdView(active: recording, level: level)
                .frame(height: 90)

            Text(timeText)
                .font(.system(size: 48, weight: .semibold, design: .rounded).monospacedDigit())
                .foregroundStyle(recording ? .red : .primary)

            ProgressView(value: level)
                .tint(level > 0.85 ? .red : .accentColor)
                .opacity(recording ? 1 : 0.3)

            HStack(spacing: 24) {
                Button {
                    model.ble.send(.mark)
                } label: {
                    Label("标记 \(ble.status?.marks ?? 0)", systemImage: "flag.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(!recording)

                Button {
                    model.toggleRecording()
                } label: {
                    Label(recording ? "停止" : "录音", systemImage: recording ? "stop.fill" : "record.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(recording ? .gray : .red)
                .disabled(!ble.isConnected || ble.status?.state == .noStorage)
            }
            .controlSize(.large)
        }
        .padding()
        .background(.background, in: RoundedRectangle(cornerRadius: 16))
    }

    private var timeText: String {
        let s = (ble.status?.elapsedMs ?? 0) / 1000
        return String(format: "%02d:%02d", s / 60, s % 60)
    }

    // MARK: Transcript

    private var transcriptCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("实时转写").font(.headline)
                Spacer()
                if model.transcriber.authorized {
                    Text(model.transcriber.onDevice ? "端侧识别" : "联网识别")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if !model.transcriber.text.isEmpty {
                    ShareLink(item: model.transcriber.text) {
                        Image(systemName: "square.and.arrow.up")
                    }
                }
            }
            if !model.decoderAvailable {
                Text("这台设备不支持 Opus 解码，无法实时转写。").foregroundStyle(.red)
            } else if !model.transcriber.authorized {
                Text("请在设置中允许 recordPan 使用语音识别。").foregroundStyle(.secondary)
            } else if model.transcriber.text.isEmpty {
                Text(recording ? "正在听…" : "开始录音后，文字会实时出现在这里。")
                    .foregroundStyle(.secondary)
            } else {
                Text(model.transcriber.text)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background, in: RoundedRectangle(cornerRadius: 16))
    }

    // MARK: Files

    private var filesCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("设备上的录音").font(.headline)
                Spacer()
                Button {
                    model.ble.refreshFiles()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(!ble.isConnected)
            }
            if ble.files.isEmpty {
                Text(ble.isConnected ? "没有录音" : "连接后显示").foregroundStyle(.secondary)
            }
            ForEach(ble.files) { file in
                HStack {
                    Image(systemName: "waveform")
                        .foregroundStyle(.secondary)
                    Text(file.name).font(.body.monospaced())
                    Spacer()
                    Text(ByteCountFormatter.string(fromByteCount: Int64(file.size), countStyle: .file))
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background, in: RoundedRectangle(cornerRadius: 16))
    }
}
