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

/// Transcribes a finished recording, 50 s at a time, as fast as the recognizer allows.
enum OfflineTranscriber {
    enum Failure: Error {
        case unavailable
    }

    static func transcribe(_ pcm: AVAudioPCMBuffer, progress: @escaping (Double) -> Void) async throws -> String {
        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "zh-CN")), recognizer.isAvailable else {
            throw Failure.unavailable
        }
        let segments = OggOpus.chunks(of: pcm, frames: AVAudioFrameCount(50 * RP.sampleRate))
        var text = ""
        for (i, segment) in segments.enumerated() {
            text += await transcribeSegment(segment, with: recognizer)
            progress(Double(i + 1) / Double(segments.count))
        }
        return text
    }

    private static func transcribeSegment(_ pcm: AVAudioPCMBuffer, with recognizer: SFSpeechRecognizer) async
        -> String {
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = false
        request.addsPunctuation = true
        if recognizer.supportsOnDeviceRecognition {
            request.requiresOnDeviceRecognition = true
        }
        for piece in OggOpus.chunks(of: pcm, frames: AVAudioFrameCount(RP.sampleRate)) {
            request.append(piece)
        }
        request.endAudio()
        return await withCheckedContinuation { continuation in
            var done = false
            _ = recognizer.recognitionTask(with: request) { result, error in
                guard !done else { return }
                if let result, result.isFinal {
                    done = true
                    continuation.resume(returning: result.bestTranscription.formattedString)
                } else if error != nil {
                    done = true  // e.g. a silent segment: nothing recognized
                    continuation.resume(returning: result?.bestTranscription.formattedString ?? "")
                }
            }
        }
    }
}
