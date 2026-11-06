import SwiftUI

/// Recordings on the phone, plus Wi-Fi sync from the device.
struct LibraryView: View {
    @EnvironmentObject private var model: AppModel

    private var store: RecordingStore { model.store }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    syncRow
                }
                Section("手机上的录音") {
                    if store.recordings.isEmpty {
                        Text("还没有录音。连上录音笔后点“从录音笔同步”。").foregroundStyle(.secondary)
                    }
                    ForEach(store.recordings) { r in
                        NavigationLink(value: r) {
                            RecordingRow(recording: r)
                        }
                    }
                    .onDelete { offsets in
                        offsets.map { store.recordings[$0] }.forEach(store.deleteLocal)
                    }
                }
            }
            .navigationTitle("录音库")
            .navigationDestination(for: LocalRecording.self) { r in
                RecordingDetailView(base: r.base)
            }
            .refreshable { store.reload() }
        }
    }

    @ViewBuilder
    private var syncRow: some View {
        Button {
            Task { await store.syncFromDevice(model.ble) }
        } label: {
            Label("从录音笔同步", systemImage: "arrow.down.circle")
        }
        .disabled(!model.ble.isConnected || store.isSyncing || model.ble.status?.state == .recording)

        switch store.sync {
        case .idle:
            Text("通过录音笔的 Wi-Fi 热点下载新录音，完成后自动断开。")
                .font(.caption).foregroundStyle(.secondary)
        case .working(let text):
            HStack { ProgressView(); Text(text) }
        case .joinManually(let ssid, let password):
            VStack(alignment: .leading, spacing: 6) {
                Text("请在“设置 → 无线局域网”里连接录音笔，连上后回到这里会自动开始下载：")
                    .font(.subheadline)
                LabeledContent("网络", value: ssid)
                LabeledContent("密码") {
                    Text(password).font(.body.monospaced()).textSelection(.enabled)
                }
                Button("复制密码") { UIPasteboard.general.string = password }
                HStack { ProgressView(); Text("等待连接…").foregroundStyle(.secondary) }
            }
        case .downloading(let done, let total):
            ProgressView(value: Double(done), total: Double(max(total, 1))) {
                Text("正在下载 \(done + 1)/\(total)")
            }
        case .finished(let count):
            Text(count == 0 ? "已是最新" : "已下载 \(count) 个录音").foregroundStyle(.green)
        case .failed(let message):
            Text(message).foregroundStyle(.red)
        }
    }
}

struct RecordingRow: View {
    let recording: LocalRecording

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(recording.base).font(.body.monospaced())
                Spacer()
                Text(formatDuration(recording.durationMs)).font(.caption).foregroundStyle(.secondary)
            }
            HStack(spacing: 12) {
                if !recording.marks.isEmpty {
                    Label("\(recording.marks.count)", systemImage: "flag.fill")
                }
                if recording.audioURL == nil {
                    Label("仅文字", systemImage: "text.alignleft")
                }
                if let t = recording.transcript {
                    Text(t).lineLimit(1)
                }
            }
            .font(.caption).foregroundStyle(.secondary)
        }
    }
}

func formatDuration(_ ms: Int) -> String {
    let s = ms / 1000
    return String(format: "%d:%02d", s / 60, s % 60)
}
