import Combine
import Foundation

/// Wires the BLE audio stream through the Opus decoder into the transcriber, starts/stops
/// transcription as the device starts/stops recording, and files the live transcript
/// under the recording it belongs to once that file shows up in the device's list.
final class AppModel: ObservableObject {
    let ble = BLEManager()
    let transcriber = Transcriber()
    let store = RecordingStore()
    private let decoder = OpusDecoder()
    private var cancellables: Set<AnyCancellable> = []
    private var wasRecording = false
    private var filesBeforeRecording: Set<String> = []
    private var pendingTranscript: String?

    init() {
        ble.onAudio = { [weak self] chunk in
            guard let self, let decoder = self.decoder else { return }
            for pcm in decoder.decode(chunk) {
                self.transcriber.append(pcm)
            }
        }
        ble.$status
            .sink { [weak self] status in self?.statusChanged(status) }
            .store(in: &cancellables)
        ble.$files
            .sink { [weak self] files in self?.filesChanged(files) }
            .store(in: &cancellables)
        // Re-publish nested changes so views observing AppModel refresh.
        for publisher in [ble.objectWillChange, transcriber.objectWillChange, store.objectWillChange] {
            publisher.sink { [weak self] in self?.objectWillChange.send() }.store(in: &cancellables)
        }
        transcriber.requestAuthorization()
    }

    var decoderAvailable: Bool { decoder != nil }

    private func statusChanged(_ status: DeviceStatus?) {
        let recording = status?.state == .recording
        if recording, !wasRecording {
            filesBeforeRecording = Set(ble.files.map(\.name))
            pendingTranscript = nil
            decoder?.reset()
            transcriber.start()
        } else if !recording, wasRecording {
            transcriber.stop()
            // Give the recognizer a moment to deliver its final words.
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                guard let self else { return }
                self.pendingTranscript = self.transcriber.text
                self.ble.refreshFiles()
            }
        }
        wasRecording = recording
    }

    private func filesChanged(_ files: [RecordingFile]) {
        guard let text = pendingTranscript, !text.isEmpty,
              let new = files.first(where: { !filesBeforeRecording.contains($0.name) }) else { return }
        store.saveTranscript(text, for: (new.name as NSString).deletingPathExtension)
        pendingTranscript = nil
    }

    func toggleRecording() {
        ble.send(ble.status?.state == .recording ? .stop : .start)
    }
}
