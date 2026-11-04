import CoreBluetooth
import Foundation

/// recordPan BLE protocol v1 — see docs/BLE.md.
enum RP {
    static func uuid(_ n: Int) -> CBUUID {
        CBUUID(string: String(format: "6A2E%04X-8B1F-4C3E-9D2A-7265636F7264", n))
    }

    static let service = uuid(1)
    static let control = uuid(2)
    static let status = uuid(3)
    static let files = uuid(4)
    static let audio = uuid(5)

    enum Command: UInt8 {
        case start = 0x01
        case stop = 0x02
        case mark = 0x03
        case listFiles = 0x04
    }

    /// Opus frame length used by the device.
    static let frameSamples = 320
    static let sampleRate = 16000.0
}

extension Data {
    func u16(at offset: Int) -> Int {
        Int(self[startIndex + offset]) | Int(self[startIndex + offset + 1]) << 8
    }

    func u32(at offset: Int) -> Int {
        u16(at: offset) | u16(at: offset + 2) << 16
    }
}

struct DeviceStatus: Equatable {
    enum State: UInt8 {
        case idle = 0
        case recording = 1
        case noStorage = 2
    }

    var state: State
    var marks: Int
    var elapsedMs: Int
    var freeKB: Int
    var peakDb: Int

    init?(_ d: Data) {
        guard d.count >= 13, d[d.startIndex] == 1, let state = State(rawValue: d[d.startIndex + 1]) else {
            return nil
        }
        self.state = state
        marks = d.u16(at: 2)
        elapsedMs = d.u32(at: 4)
        freeKB = d.u32(at: 8)
        peakDb = Int(Int8(bitPattern: d[d.startIndex + 12]))
    }

    /// Remaining recording time at 24 kbps Opus (~3 KB/s).
    var remainingMinutes: Int { freeKB / 3 / 60 }
}

struct RecordingFile: Identifiable, Hashable {
    let name: String
    let size: Int
    var id: String { name }
}

/// One audio notification: frame index of the first packet plus up to 3 Opus packets.
struct AudioChunk {
    let firstFrame: UInt16
    let packets: [Data]

    init?(_ d: Data) {
        guard d.count >= 3 else { return nil }
        firstFrame = UInt16(d.u16(at: 0))
        var packets: [Data] = []
        var pos = 3
        for _ in 0..<Int(d[d.startIndex + 2]) {
            guard pos < d.count else { return nil }
            let len = Int(d[d.startIndex + pos])
            guard pos + 1 + len <= d.count else { return nil }
            packets.append(d.subdata(in: (d.startIndex + pos + 1)..<(d.startIndex + pos + 1 + len)))
            pos += 1 + len
        }
        self.packets = packets
    }
}
