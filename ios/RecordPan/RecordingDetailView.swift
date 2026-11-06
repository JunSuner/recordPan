import SwiftUI

/// Playback with mark shortcuts, and the transcript (from live recording or made offline).
struct RecordingDetailView: View {
    @EnvironmentObject private var model: AppModel
    @StateObject private var player = Player()
    @Environment(\.dismiss) private var dismiss
    let base: String

    @State private var loadError: String?
    @State private var transcribing = false
    @State private var progress = 0.0
    @State private var confirmDeviceDelete = false

    private var recording: LocalRecording? { model.store.recordings.first { $0.base == base } }

    var body: some View {
        List {
            if let r = recording {
                if r.audioURL != nil {
                    Section { playerControls(r) }
                }
                if !r.marks.isEmpty {
                    Section("标记") { marks(r) }
                }
                Section {
                    transcript(r)
                } header: {
                    Text("转写")
                }
                Section {
                    if model.ble.files.contains(where: { $0.name.hasPrefix(r.base + ".") }) {
                        Button("从录音笔上删除", role: .destructive) { confirmDeviceDelete = true }
                            .disabled(!model.ble.isConnected)
                    }
                    Button("从手机上删除", role: .destructive) {
                        player.stop()
                        model.store.deleteLocal(r)
                        dismiss()
                    }
                }
            }
        }
        .navigationTitle(base)
        .navigationBarTitleDisplayMode(.inline)
        .task { load() }
        .onDisappear { player.stop() }
        .confirmationDialog("从录音笔上删除 \(base)？手机上的副本会保留。", isPresented: $confirmDeviceDelete,
                            titleVisibility: .visible) {
            Button("删除", role: .destructive) {
                for f in model.ble.files where f.name.hasPrefix(base + ".") {
                    model.ble.delete(f.name)
                }
            }
        }
    }

    private func load() {
        guard let r = recording, r.audioURL != nil else { return }
        do {
            try player.load(try model.store.playableURL(for: r))
        } catch {
            loadError = "无法打开录音：\(error.localizedDescription)"
        }
    }

    @ViewBuilder
    private func playerControls(_ r: LocalRecording) -> some View {
        if let loadError {
            Text(loadError).foregroundStyle(.red)
        } else {
            VStack(spacing: 8) {
                Slider(value: Binding(get: { player.current }, set: { player.seek(to: $0) }),
                       in: 0...max(player.duration, 0.1))
                HStack {
                    Text(formatDuration(Int(player.current * 1000))).monospacedDigit()
                    Spacer()
                    Button {
                        player.toggle()
                    } label: {
                        Image(systemName: player.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                            .font(.system(size: 44))
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    Text(formatDuration(Int(player.duration * 1000))).monospacedDigit()
                }
                .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private func marks(_ r: LocalRecording) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack {
                ForEach(Array(r.marks.enumerated()), id: \.offset) { i, ms in
                    Button {
                        player.seek(to: max(0, Double(ms) / 1000 - 2))  // start a little before the mark
                        if !player.isPlaying { player.toggle() }
                    } label: {
                        Label("\(i + 1) · \(formatDuration(ms))", systemImage: "flag.fill")
                    }
                    .buttonStyle(.bordered)
                    .disabled(r.audioURL == nil)
                }
            }
        }
    }

    @ViewBuilder
    private func transcript(_ r: LocalRecording) -> some View {
        if let text = r.transcript, !text.isEmpty {
            Text(text).textSelection(.enabled)
            ShareLink(item: text) { Label("分享文字", systemImage: "square.and.arrow.up") }
        } else if !transcribing {
            Text("还没有转写。").foregroundStyle(.secondary)
        }
        if r.audioURL != nil {
            if transcribing {
                ProgressView(value: progress) { Text("正在转写…") }
            } else {
                Button(r.transcript == nil ? "转写这段录音" : "重新转写") { transcribe(r) }
                    .disabled(!model.transcriber.authorized)
            }
        }
    }

    private func transcribe(_ r: LocalRecording) {
        transcribing = true
        progress = 0
        Task {
            defer { transcribing = false }
            do {
                let pcm = try model.store.pcm(for: r)
                let text = try await OfflineTranscriber.transcribe(pcm) { p in
                    DispatchQueue.main.async { progress = p }
                }
                model.store.saveTranscript(text.isEmpty ? "（没有识别到语音）" : text, for: r.base)
            } catch {
                loadError = "转写失败：\(error.localizedDescription)"
            }
        }
    }
}
