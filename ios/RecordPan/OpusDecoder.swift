import AVFoundation

/// Decodes the device's 20 ms, 16 kHz mono Opus packets to Float32 PCM with Core Audio's
/// built-in Opus decoder, filling gaps in the frame sequence with silence.
final class OpusDecoder {
    let pcmFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: RP.sampleRate,
                                  channels: 1, interleaved: false)!
    private let opusFormat: AVAudioFormat
    private let converter: AVAudioConverter
    private var nextFrame: UInt16?

    init?() {
        var desc = AudioStreamBasicDescription(
            mSampleRate: RP.sampleRate, mFormatID: kAudioFormatOpus, mFormatFlags: 0,
            mBytesPerPacket: 0, mFramesPerPacket: UInt32(RP.frameSamples), mBytesPerFrame: 0,
            mChannelsPerFrame: 1, mBitsPerChannel: 0, mReserved: 0)
        guard let opus = AVAudioFormat(streamDescription: &desc),
              let converter = AVAudioConverter(from: opus, to: pcmFormat) else {
            return nil
        }
        opusFormat = opus
        self.converter = converter
    }

    /// Call when a new recording starts.
    func reset() {
        converter.reset()
        nextFrame = nil
    }

    /// Returns PCM for the chunk, preceded by silence for any frames lost on the way.
    func decode(_ chunk: AudioChunk) -> [AVAudioPCMBuffer] {
        var out: [AVAudioPCMBuffer] = []
        if let expected = nextFrame {
            let missing = Int(chunk.firstFrame &- expected)
            if missing > 0, missing < 50, let silence = makeSilence(frames: missing) {
                out.append(silence)
            }
        }
        for packet in chunk.packets {
            if let pcm = decode(packet) {
                out.append(pcm)
            }
        }
        nextFrame = chunk.firstFrame &+ UInt16(chunk.packets.count)
        return out
    }

    private func decode(_ packet: Data) -> AVAudioPCMBuffer? {
        let compressed = AVAudioCompressedBuffer(format: opusFormat, packetCapacity: 1,
                                                 maximumPacketSize: max(packet.count, 1))
        packet.withUnsafeBytes { compressed.data.copyMemory(from: $0.baseAddress!, byteCount: packet.count) }
        compressed.byteLength = UInt32(packet.count)
        compressed.packetCount = 1
        compressed.packetDescriptions?[0] = AudioStreamPacketDescription(
            mStartOffset: 0, mVariableFramesInPacket: 0, mDataByteSize: UInt32(packet.count))

        guard let pcm = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: AVAudioFrameCount(RP.frameSamples))
        else { return nil }
        var fed = false
        var error: NSError?
        let status = converter.convert(to: pcm, error: &error) { _, inputStatus in
            if fed {
                inputStatus.pointee = .noDataNow
                return nil
            }
            fed = true
            inputStatus.pointee = .haveData
            return compressed
        }
        return status == .error || pcm.frameLength == 0 ? nil : pcm
    }

    private func makeSilence(frames: Int) -> AVAudioPCMBuffer? {
        let count = AVAudioFrameCount(frames * RP.frameSamples)
        guard let pcm = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: count) else { return nil }
        pcm.frameLength = count  // buffers start zeroed
        return pcm
    }
}
