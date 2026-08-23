import SwiftUI

@main
struct fing_labApp: App {
    @State private var coordinator = RecordingCoordinator()
    @State private var settings = AppSettings()

    init() {
        FeedbackManager.prepare()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(coordinator)
                .environment(settings)
        }
    }
}
