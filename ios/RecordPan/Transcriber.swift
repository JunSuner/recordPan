import AVFoundation
import Combine
import Speech

/// Live Mandarin transcription of streamed PCM with SFSpeechRecognizer.
/// Long recordings are split into ~50 s segments, each with its own recognition
/// task, so no request runs into the recognizer's duration limit.
final class Transcriber: ObservableObject {
    @Published private(set) var segments: [String] = []
    @Published private(set) var authorized = false
    @Published private(set) var onDevice = false

    var text: String { segments.joined() }

    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "zh-CN"))
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var tasks: [SFSpeechRecognitionTask] = []
    private var segmentFrames: AVAudioFrameCount = 0
    private let segmentLimit = AVAudioFrameCount(50 * RP.sampleRate)

    func requestAuthorization() {
        SFSpeechRecognizer.requestAuthorization { status in
            DispatchQueue.main.async {
                self.authorized = status == .authorized
                self.onDevice = self.recognizer?.supportsOnDeviceRecognition ?? false
            }
        }
    }

    func start() {
        stop()
        segments = []
        startSegment()
    }

    func append(_ buffer: AVAudioPCMBuffer) {
        guard let request else { return }
        request.append(buffer)
        segmentFrames += buffer.frameLength
        if segmentFrames >= segmentLimit {
            request.endAudio()  // its task delivers the final text for this segment
            startSegment()
        }
    }

    func stop() {
        request?.endAudio()
        request = nil
    }

    private func startSegment() {
        guard authorized, let recognizer, recognizer.isAvailable else { return }
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true
        request.addsPunctuation = true
        if recognizer.supportsOnDeviceRecognition {
            request.requiresOnDeviceRecognition = true
        }
        let index = segments.count
        segments.append("")
        segmentFrames = 0
        self.request = request
        let task = recognizer.recognitionTask(with: request) { [weak self] result, _ in
            guard let result else { return }
            let text = result.bestTranscription.formattedString
            DispatchQueue.main.async {
                guard let self, index < self.segments.count else { return }
                self.segments[index] = text
            }
        }
        tasks.append(task)
    }
}
