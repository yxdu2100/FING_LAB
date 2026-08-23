import Foundation
import Observation

struct EverydayTaskDefinition: Identifiable, Hashable {
    let id: String
    let name: String
    let instruction: String

    static let defaults: [EverydayTaskDefinition] = [
        .init(id: "typing", name: "Typing", instruction: "Type naturally on a keyboard for two minutes."),
        .init(id: "phone_use", name: "Phone Use", instruction: "Use the phone naturally for two minutes."),
        .init(id: "writing", name: "Writing", instruction: "Write naturally with a pen for two minutes."),
        .init(id: "walking", name: "Walking", instruction: "Walk normally for two minutes."),
        .init(id: "tooth_brushing", name: "Tooth Brushing", instruction: "Brush your teeth normally for two minutes."),
    ]
}

enum EverydayPhase: Equatable {
    case taskIntro
    case countdown(Int)
    case recording
    case paused
    case taskComplete
    case sessionComplete
}

@Observable
@MainActor
final class EverydayTasksViewModel {
    let coordinator: RecordingCoordinator
    let settings: AppSettings

    private(set) var tasks = EverydayTaskDefinition.defaults
    private(set) var currentTaskIndex = 0
    private(set) var phase: EverydayPhase = .taskIntro
    private(set) var secondsRemaining = 120
    private(set) var errorMessage: String?

    private var taskRunner: Task<Void, Never>?
    private var phaseBeforePause: EverydayPhase?
    private var currentTaskStartSampleID: UInt64?
    private let taskDurationSeconds = 120

    var currentTask: EverydayTaskDefinition? {
        guard currentTaskIndex < tasks.count else { return nil }
        return tasks[currentTaskIndex]
    }

    var progressLabel: String {
        "Task \(currentTaskIndex + 1) of \(tasks.count)"
    }

    init(coordinator: RecordingCoordinator, settings: AppSettings) {
        self.coordinator = coordinator
        self.settings = settings
    }

    func startSession() {
        errorMessage = nil
        do {
            try coordinator.beginSession(
                participantID: settings.participantID,
                mode: .everyday,
                gestureSetVersion: settings.gestureSetVersion,
                imuConfig: settings.imuConfig,
                notes: "everyday_task_duration_seconds=120"
            )
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func startCurrentTask() {
        guard phase == .taskIntro, currentTask != nil else { return }
        currentTaskStartSampleID = nil
        taskRunner?.cancel()
        taskRunner = Task { [weak self] in
            await self?.runCurrentTask()
        }
    }

    func pause() {
        switch phase {
        case .countdown, .recording:
            phaseBeforePause = phase
        default:
            return
        }

        taskRunner?.cancel()
        if phase == .recording, let task = currentTask {
            coordinator.writeMarker(eventType: "task_pause", label: task.id)
        }
        phase = .paused
        FeedbackManager.pauseToggle()
    }

    func resume() {
        guard phase == .paused else { return }
        let previousPhase = phaseBeforePause
        phaseBeforePause = nil

        if previousPhase == .recording, let task = currentTask {
            coordinator.writeMarker(eventType: "task_resume", label: task.id)
            phase = .recording
            taskRunner = Task { [weak self] in
                await self?.runRemainingRecordingTime()
            }
        } else {
            // A paused countdown restarts from three so the participant gets
            // a complete cue before any samples are labeled as this task.
            phase = .taskIntro
            startCurrentTask()
        }
        FeedbackManager.pauseToggle()
    }

    func redoCurrentTask() {
        guard phase != .taskIntro, phase != .sessionComplete,
              let task = currentTask else { return }

        taskRunner?.cancel()
        if let invalidatedStart = currentTaskStartSampleID {
            // Raw samples remain available for auditing, while this marker
            // identifies the invalid task span that analysis should exclude.
            coordinator.writeMarker(
                eventType: "task_redo",
                label: task.id,
                invalidatedCueUnwrappedSampleID: invalidatedStart
            )
        } else {
            coordinator.writeMarker(eventType: "task_countdown_cancelled", label: task.id)
        }

        currentTaskStartSampleID = nil
        phaseBeforePause = nil
        secondsRemaining = taskDurationSeconds
        phase = .taskIntro
        FeedbackManager.warning()
    }

    func advance() {
        guard phase == .taskComplete else { return }
        currentTaskIndex += 1
        currentTaskStartSampleID = nil
        phaseBeforePause = nil
        if currentTaskIndex >= tasks.count {
            finishSession()
        } else {
            secondsRemaining = taskDurationSeconds
            phase = .taskIntro
        }
    }

    func cancel() {
        taskRunner?.cancel()
        coordinator.cancelSession()
    }

    private func runCurrentTask() async {
        for tick in stride(from: 3, through: 1, by: -1) {
            phase = .countdown(tick)
            FeedbackManager.countdownTick()
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
        }

        guard let currentTask else { return }
        secondsRemaining = taskDurationSeconds
        currentTaskStartSampleID = coordinator.writeMarker(
            eventType: "task_start",
            label: currentTask.id
        )
        FeedbackManager.goCue()
        phase = .recording

        await runRemainingRecordingTime()
    }

    private func runRemainingRecordingTime() async {
        while secondsRemaining > 0 {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            secondsRemaining -= 1
        }

        guard let currentTask else { return }
        coordinator.writeMarker(eventType: "task_end", label: currentTask.id)
        FeedbackManager.blockStart()
        phase = .taskComplete
    }

    private func finishSession() {
        do {
            try coordinator.endSession()
            phase = .sessionComplete
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
