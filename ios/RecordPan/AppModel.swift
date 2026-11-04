import Combine
import Foundation

/// Wires the BLE audio stream through the Opus decoder into the transcriber, and
/// starts/stops transcription as the device starts/stops recording.
final class AppModel: ObservableObject {
    let ble = BLEManager()
    let transcriber = Transcriber()
    private let decoder = OpusDecoder()
    private var cancellables: Set<AnyCancellable> = []
    private var wasRecording = false

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
        // Re-publish nested changes so views observing AppModel refresh.
        ble.objectWillChange.sink { [weak self] in self?.objectWillChange.send() }.store(in: &cancellables)
        transcriber.objectWillChange.sink { [weak self] in self?.objectWillChange.send() }.store(in: &cancellables)
        transcriber.requestAuthorization()
    }

    var decoderAvailable: Bool { decoder != nil }

    private func statusChanged(_ status: DeviceStatus?) {
        let recording = status?.state == .recording
        if recording, !wasRecording {
            decoder?.reset()
            transcriber.start()
        } else if !recording, wasRecording {
            transcriber.stop()
        }
        wasRecording = recording
    }

    func toggleRecording() {
        ble.send(ble.status?.state == .recording ? .stop : .start)
    }
}
