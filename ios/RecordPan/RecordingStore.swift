import AVFoundation
import Combine
import Foundation

/// REC_NNNN.json written by the device.
struct RecordingMeta: Codable, Hashable {
    var audio: String
    var sample_rate: Int
    var channels: Int
    var duration_ms: Int
    var marks_ms: [Int]
}

/// A recording kept on the phone: Documents/Recordings/REC_NNNN.{opus,json,txt}.
struct LocalRecording: Identifiable, Hashable {
    let base: String
    var audioURL: URL?
    var meta: RecordingMeta?
    var transcript: String?

    var id: String { base }
    var durationMs: Int { meta?.duration_ms ?? 0 }
    var marks: [Int] { meta?.marks_ms ?? [] }
}

/// The phone-side library, and bulk sync from the device over its Wi-Fi access point
/// (HTTP API in docs/WIFI.md).
final class RecordingStore: ObservableObject {
    enum SyncState: Equatable {
        case idle
        case working(String)
        case joinManually(ssid: String, password: String)
        case downloading(done: Int, total: Int)
        case finished(Int)
        case failed(String)
    }

    @Published private(set) var recordings: [LocalRecording] = []
    @Published private(set) var sync: SyncState = .idle

    let directory: URL
    private let cacheDirectory: URL
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 15
        return URLSession(configuration: config)
    }()

    init() {
        let fm = FileManager.default
        directory = fm.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Recordings")
        cacheDirectory = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        reload()
    }

    var isSyncing: Bool {
        switch sync {
        case .working, .joinManually, .downloading: true
        default: false
        }
    }

    func reload() {
        let urls = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
        var byBase: [String: LocalRecording] = [:]
        for url in urls {
            let base = url.deletingPathExtension().lastPathComponent
            var r = byBase[base] ?? LocalRecording(base: base)
            switch url.pathExtension.lowercased() {
            case "opus", "wav": r.audioURL = url
            case "json": r.meta = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(RecordingMeta.self, from: $0) }
            case "txt": r.transcript = try? String(contentsOf: url, encoding: .utf8)
            default: continue
            }
            byBase[base] = r
        }
        recordings = byBase.values.sorted { $0.base > $1.base }
    }

    func saveTranscript(_ text: String, for base: String) {
        guard !text.isEmpty else { return }
        try? text.write(to: directory.appendingPathComponent(base + ".txt"), atomically: true, encoding: .utf8)
        reload()
    }

    func deleteLocal(_ r: LocalRecording) {
        let fm = FileManager.default
        for ext in ["opus", "wav", "json", "txt"] {
            try? fm.removeItem(at: directory.appendingPathComponent("\(r.base).\(ext)"))
        }
        try? fm.removeItem(at: cacheURL(for: r))
        reload()
    }

    private func cacheURL(for r: LocalRecording) -> URL {
        cacheDirectory.appendingPathComponent(r.base + ".wav")
    }

    /// A seekable WAV for playback, decoded from the Opus file once and cached.
    func playableURL(for r: LocalRecording) throws -> URL {
        guard let audio = r.audioURL else { throw CocoaError(.fileNoSuchFile) }
        if audio.pathExtension.lowercased() == "wav" {
            return audio
        }
        let wav = cacheURL(for: r)
        if !FileManager.default.fileExists(atPath: wav.path) {
            try OggOpus.writeWAV(try OggOpus.decode(fileAt: audio), to: wav)
        }
        return wav
    }

    func pcm(for r: LocalRecording) throws -> AVAudioPCMBuffer {
        let file = try AVAudioFile(forReading: try playableURL(for: r), commonFormat: .pcmFormatFloat32,
                                   interleaved: false)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat,
                                            frameCapacity: AVAudioFrameCount(file.length)) else {
            throw CocoaError(.fileReadUnknown)
        }
        try file.read(into: buffer)
        return buffer
    }

    // MARK: Sync

    private struct ListResponse: Decodable {
        struct Entry: Decodable {
            let name: String
            let size: Int
            let meta: RecordingMeta?
        }

        let recording: String?
        let files: [Entry]
    }

    /// Turns on the device's access point over BLE, joins it, downloads every recording
    /// the phone doesn't have yet, then leaves the network and turns Wi-Fi off again.
    @MainActor
    func syncFromDevice(_ ble: BLEManager) async {
        guard let wifi = ble.wifi else {
            sync = .failed("还没拿到录音笔的 Wi-Fi 信息，请先连上蓝牙")
            return
        }
        sync = .working("正在打开录音笔的 Wi-Fi…")
        ble.send(.wifiOn)
        for _ in 0..<40 where ble.status?.wifiOn != true {
            try? await Task.sleep(for: .milliseconds(250))
        }
        defer { ble.send(.wifiOff) }

        // Joining from the app (NEHotspotConfiguration) needs a capability free Apple IDs
        // don't get (without it the call never returns), so the user joins in Settings
        // and we poll until the device's file server answers.
        let base = URL(string: "http://\(wifi.host)")!
        var list: ListResponse?
        for attempt in 0..<180 {
            if attempt == 1 {
                sync = .joinManually(ssid: wifi.ssid, password: wifi.password)
            }
            var request = URLRequest(url: base.appendingPathComponent("api/files"))
            request.timeoutInterval = 3  // fail fast while the phone isn't on the device's network
            if let (data, _) = try? await session.data(for: request) {
                list = try? JSONDecoder().decode(ListResponse.self, from: data)
                if list != nil { break }
            }
            if Task.isCancelled { break }
            try? await Task.sleep(for: .seconds(1))
        }
        guard let list else {
            sync = .failed("连不上录音笔的 Wi-Fi，请确认手机已连到 \(wifi.ssid)")
            return
        }

        let have = Set(recordings.filter { $0.audioURL != nil }.map(\.base))
        let missing = list.files.filter { !have.contains(($0.name as NSString).deletingPathExtension) }
        for (i, entry) in missing.enumerated() {
            sync = .downloading(done: i, total: missing.count)
            do {
                let (tmp, response) = try await session.download(from: base.appendingPathComponent("files/\(entry.name)"))
                guard (response as? HTTPURLResponse)?.statusCode == 200 else { continue }
                let dest = directory.appendingPathComponent(entry.name)
                try? FileManager.default.removeItem(at: dest)
                try FileManager.default.moveItem(at: tmp, to: dest)
                if let meta = entry.meta, let json = try? JSONEncoder().encode(meta) {
                    let metaURL = directory.appendingPathComponent((entry.name as NSString).deletingPathExtension + ".json")
                    try? json.write(to: metaURL)
                }
            } catch {
                sync = .failed("下载 \(entry.name) 失败：\(error.localizedDescription)")
                reload()
                return
            }
        }
        reload()
        sync = .finished(missing.count)
    }
}
