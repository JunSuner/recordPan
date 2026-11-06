import AVFoundation
import Combine

/// Plays one recording and publishes its position for the UI.
final class Player: ObservableObject {
    @Published private(set) var isPlaying = false
    @Published private(set) var current: TimeInterval = 0
    @Published private(set) var duration: TimeInterval = 0

    private var player: AVAudioPlayer?
    private var timer: AnyCancellable?

    func load(_ url: URL) throws {
        stop()
        try AVAudioSession.sharedInstance().setCategory(.playback)
        let p = try AVAudioPlayer(contentsOf: url)
        p.prepareToPlay()
        player = p
        duration = p.duration
        current = 0
    }

    func toggle() {
        guard let player else { return }
        if player.isPlaying {
            player.pause()
        } else {
            try? AVAudioSession.sharedInstance().setActive(true)
            player.play()
        }
        update()
        timer = player.isPlaying
            ? Timer.publish(every: 0.2, on: .main, in: .common).autoconnect().sink { [weak self] _ in self?.update() }
            : nil
    }

    func seek(to seconds: TimeInterval) {
        player?.currentTime = max(0, min(seconds, duration))
        update()
    }

    func stop() {
        player?.stop()
        player = nil
        timer = nil
        isPlaying = false
    }

    private func update() {
        current = player?.currentTime ?? 0
        isPlaying = player?.isPlaying ?? false
        if !isPlaying {
            timer = nil
        }
    }
}
