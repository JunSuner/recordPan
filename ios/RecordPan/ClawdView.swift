import SwiftUI

/// Clawd drawn from the same block anatomy as the device's OLED: an 11x7 torso,
/// 2x2 hands, four 1x2 legs and a shadow on a 15x10 grid. Bounces while `active`.
struct ClawdView: View {
    var active: Bool
    var level: Double = 0  // 0...1, opens the mouth while talking

    private static let bodyColor = Color(red: 0xDE / 255, green: 0x88 / 255, blue: 0x6D / 255)
    private static let mouth = Color(red: 0x7A / 255, green: 0x22 / 255, blue: 0x30 / 255)

    var body: some View {
        TimelineView(.animation(minimumInterval: 0.1)) { context in
            let t = context.date.timeIntervalSinceReferenceDate
            let beat = Int(t * 5) % 2 == 0
            let blink = Int(t * 10) % 35 == 0
            Canvas { ctx, size in
                let u = min(size.width / 15, size.height / 11)  // one spare row above for bouncing
                let ox = (size.width - 15 * u) / 2
                let lift = active ? (beat ? 0.6 * u : 0) : (Int(t) % 2 == 0 ? 0.25 * u : 0)
                func rect(_ x: Double, _ y: Double, _ w: Double, _ h: Double, _ color: Color, dy: Double = 0) {
                    ctx.fill(Path(CGRect(x: ox + x * u, y: (y + 1) * u - dy, width: w * u, height: h * u)), with: .color(color))
                }
                rect(3, 9.4, 9, 0.5, .black.opacity(0.25))  // shadow stays on the ground
                rect(2, 0, 11, 7, Self.bodyColor, dy: lift)
                for x in [3.0, 5, 9, 11] {
                    rect(x, 7, 1, 2, Self.bodyColor, dy: lift)
                }
                let leftUp = active && beat, rightUp = active && !beat
                rect(0, leftUp ? 1 : 3, 2, 2, Self.bodyColor, dy: lift)
                rect(13, rightUp ? 1 : 3, 2, 2, Self.bodyColor, dy: lift)
                let eyeH = blink ? 0.3 : 2
                rect(4, 2 + (2 - eyeH), 1, eyeH, .black, dy: lift)
                rect(10, 2 + (2 - eyeH), 1, eyeH, .black, dy: lift)
                if active {
                    rect(6.4, 4.8, 2.2, 0.4 + 1.2 * level, Self.mouth, dy: lift)
                }
            }
        }
        .aspectRatio(15.0 / 11, contentMode: .fit)
        .accessibilityLabel("Clawd")
    }
}

#Preview {
    VStack {
        ClawdView(active: false)
        ClawdView(active: true, level: 0.8)
    }
    .padding()
}
