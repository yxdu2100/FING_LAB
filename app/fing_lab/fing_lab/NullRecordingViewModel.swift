import Foundation

@Observable
@MainActor
final class NullRecordingViewModel {
    let coordinator: RecordingCoordinator
    let settings: AppSettings

    private(set) var isRecording = false
    private(set) var elapsed: TimeInterval = 0
    private(set) var errorMessage: String?

    private var timer: Timer?
    private var startDate: Date?

    init(coordinator: RecordingCoordinator, settings: AppSettings) {
        self.coordinator = coordinator
        self.settings = settings
    }

    func start() {
        errorMessage = nil
        do {
            try coordinator.beginSession(
                participantID: settings.participantID,
                mode: .null,
                gestureSetVersion: settings.gestureSetVersion,
                imuConfig: settings.imuConfig
            )
            isRecording = true
            startDate = Date()
            elapsed = 0
            startTimer()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func stop() async {
        stopTimer()
        do {
            try coordinator.endSession()
            isRecording = false
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func cancel() {
        stopTimer()
        coordinator.cancelSession()
        isRecording = false
    }

    private func startTimer() {
        stopTimer()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self, let start = self.startDate else { return }
            Task { @MainActor in
                self.elapsed = Date().timeIntervalSince(start)
            }
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }
}
