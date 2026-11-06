import AVFoundation

/// Reads the device's .opus files (RFC 7845 Ogg Opus, one logical stream) and
/// decodes them to 16 kHz mono PCM.
enum OggOpus {
    struct Stream {
        var preSkip48k = 0
        var packets: [Data] = []
    }

    /// Splits the file into Opus packets, dropping the OpusHead/OpusTags headers.
    static func parse(_ data: Data) -> Stream? {
        var stream = Stream()
        var all: [Data] = []
        var partial = Data()
        var pos = data.startIndex
        while pos + 27 <= data.endIndex {
            guard data[pos..<pos + 4] == Data("OggS".utf8) else { return nil }
            let segCount = Int(data[pos + 26])
            let lacingStart = pos + 27
            guard lacingStart + segCount <= data.endIndex else { break }
            var bodyPos = lacingStart + segCount
            for i in 0..<segCount {
                let len = Int(data[lacingStart + i])
                guard bodyPos + len <= data.endIndex else { break }
                partial.append(data[bodyPos..<bodyPos + len])
                bodyPos += len
                if len < 255 {  // a lacing value below 255 ends the packet
                    all.append(partial)
                    partial = Data()
                }
            }
            pos = bodyPos
        }
        guard all.count >= 2, all[0].starts(with: Data("OpusHead".utf8)), all[0].count >= 12 else { return nil }
        stream.preSkip48k = Int(all[0][all[0].startIndex + 10]) | Int(all[0][all[0].startIndex + 11]) << 8
        stream.packets = Array(all.dropFirst(2))
        return stream
    }

    /// Decodes a whole file to one PCM buffer (encoder lookahead trimmed).
    static func decode(fileAt url: URL) throws -> AVAudioPCMBuffer {
        let data = try Data(contentsOf: url)
        guard let stream = parse(data), let decoder = OpusDecoder() else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let skip = AVAudioFrameCount(stream.preSkip48k / 3)  // 48 kHz -> 16 kHz samples
        let capacity = AVAudioFrameCount(stream.packets.count * RP.frameSamples)
        guard let out = AVAudioPCMBuffer(pcmFormat: decoder.pcmFormat, frameCapacity: max(capacity, 1)) else {
            throw CocoaError(.fileReadUnknown)
        }
        var skipLeft = skip
        for packet in stream.packets {
            guard let pcm = decoder.decode(packet) else { continue }
            let n = pcm.frameLength
            let drop = min(skipLeft, n)
            skipLeft -= drop
            let keep = n - drop
            guard keep > 0, out.frameLength + keep <= out.frameCapacity else { continue }
            memcpy(out.floatChannelData![0] + Int(out.frameLength), pcm.floatChannelData![0] + Int(drop),
                   Int(keep) * MemoryLayout<Float>.size)
            out.frameLength += keep
        }
        return out
    }

    /// Writes PCM as a 16-bit WAV that AVAudioPlayer can play and seek.
    static func writeWAV(_ pcm: AVAudioPCMBuffer, to url: URL) throws {
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: RP.sampleRate, AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
        ]
        let file = try AVAudioFile(forWriting: url, settings: settings, commonFormat: .pcmFormatFloat32,
                                   interleaved: false)
        try file.write(from: pcm)
    }

    /// Splits a long buffer into pieces of at most `frames` frames.
    static func chunks(of pcm: AVAudioPCMBuffer, frames: AVAudioFrameCount) -> [AVAudioPCMBuffer] {
        var result: [AVAudioPCMBuffer] = []
        var start: AVAudioFrameCount = 0
        while start < pcm.frameLength {
            let n = min(frames, pcm.frameLength - start)
            guard let piece = AVAudioPCMBuffer(pcmFormat: pcm.format, frameCapacity: n) else { break }
            memcpy(piece.floatChannelData![0], pcm.floatChannelData![0] + Int(start), Int(n) * MemoryLayout<Float>.size)
            piece.frameLength = n
            result.append(piece)
            start += n
        }
        return result
    }
}
