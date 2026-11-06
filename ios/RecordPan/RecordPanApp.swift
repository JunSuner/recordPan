import SwiftUI

@main
struct RecordPanApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            TabView {
                Tab("录音笔", systemImage: "mic.fill") {
                    ContentView()
                }
                Tab("录音库", systemImage: "list.bullet.rectangle") {
                    LibraryView()
                }
            }
            .environmentObject(model)
        }
    }
}
