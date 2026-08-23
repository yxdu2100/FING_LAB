import Foundation
import Observation

enum GuidedPhase: Equatable {
    case blockIntro
    case countdown(Int)
    case goFlash
    case perform
    case rest
    case blockComplete
    case redonPrompt
    case sessionComplete
    case paused
}

struct GuidedBlock: Identifiable, Hashable {
    let id: String
    let gesture: GestureDefinition
    let sessionIndex: Int
}

@Observable
@MainActor
final class GuidedSessionViewModel {
    let coordinator: RecordingCoordinator
    let settings: AppSettings

    private(set) var blocks: [GuidedBlock] = []
    private(set) var currentBlockIndex = 0
    private(set) var currentRep = 0
    private(set) var phase: GuidedPhase = .blockIntro
    private(set) var performProgress: Double = 0
    private(set) var lastCueLabel: String?
    private(set) var errorMessage: String?

    private var phaseTask: Task<Void, Never>?
    private var lastGoCueSampleID: UInt64?
    /// Reps added to the current block via `redoLastRep()`, so a marked-bad rep gets
    /// backfilled with a clean one instead of just being annotated and left short.
    private var extraRepsForBlock = 0

    let performDuration: TimeInterval = 2.5

    private var countdownStart: Int {
        settings.fastGuidedMode ? 1 : 3
    }

    private var restDuration: TimeInterval {
        settings.fastGuidedMode ? 0.5 : 1.5
    }

    var currentGesture: GestureDefinition? {
        guard currentBlockIndex < blocks.count else { return nil }
        return blocks[currentBlockIndex].gesture
    }

    var currentGestureSession: Int {
        guard currentBlockIndex < blocks.count else { return settings.sessionsPerGesture }
        return blocks[currentBlockIndex].sessionIndex + 1
    }

    var repsPerBlock: Int { settings.repsPerBlock }

    /// Effective rep target for the current block, including any redo backfill.
    var repsTarget: Int { repsPerBlock + extraRepsForBlock }

    var blockProgressLabel: String {
        "Session \(currentGestureSession)/\(settings.sessionsPerGesture) · "
        + "Block \(currentBlockIndex + 1)/\(blocks.count) · "
        + "Rep \(min(currentRep + 1, repsTarget))/\(repsTarget)"
    }

    init(coordinator: RecordingCoordinator, settings: AppSettings) {
        self.coordinator = coordinator
        self.settings = settings
    }

    func start() {
        blocks = (0..<settings.sessionsPerGesture).flatMap { sessionIndex in
            settings.enabledGestures.shuffled().map { gesture in
                GuidedBlock(
                    id: "session_\(sessionIndex + 1)_\(gesture.id)",
                    gesture: gesture,
                    sessionIndex: sessionIndex
                )
            }
        }
        currentBlockIndex = 0
        currentRep = 0
        extraRepsForBlock = 0
        phase = .blockIntro
        errorMessage = nil

        do {
            try coordinator.beginSession(
                participantID: settings.participantID,
                mode: .guided,
                gestureSetVersion: settings.gestureSetVersion,
                imuConfig: settings.imuConfig,
                notes: [
                    "repetitions_per_block=\(settings.repsPerBlock)",
                    "sessions_per_gesture=\(settings.sessionsPerGesture)",
                    settings.fastGuidedMode ? "guided_fast_mode=true" : nil,
                ].compactMap { $0 }.joined(separator: ";")
            )
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func startBlock() {
        guard currentGesture != nil else { return }
        FeedbackManager.blockStart()
        if isFirstBlockInCurrentSession {
            coordinator.writeMarker(
                eventType: "gesture_session_start",
                label: String(currentGestureSession)
            )
        }
        coordinator.writeMarker(eventType: "block_start", label: currentGesture?.id ?? "")
        runRepLoop()
    }

    func pause() {
        guard phase != .paused, phase != .sessionComplete else { return }
        phaseTask?.cancel()
        phase = .paused
        FeedbackManager.pauseToggle()
    }

    func resume() {
        guard phase == .paused else { return }
        runRepLoop()
    }

    func redoLastRep() {
        guard let label = lastCueLabel, let invalidatedCue = lastGoCueSampleID else { return }
        coordinator.writeMarker(
            eventType: "redo",
            label: label,
            invalidatedCueUnwrappedSampleID: invalidatedCue
        )
        extraRepsForBlock += 1
        FeedbackManager.warning()
    }

    func confirmRedon() {
        coordinator.writeMarker(eventType: "redon", label: "redon")
        advanceToNextBlock()
    }

    func skipRedon() {
        advanceToNextBlock()
    }

    func finishSession() async {
        phaseTask?.cancel()
        do {
            try coordinator.endSession()
            phase = .sessionComplete
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func cancel() {
        phaseTask?.cancel()
        coordinator.cancelSession()
    }

    private func runRepLoop() {
        phaseTask?.cancel()
        phaseTask = Task { [weak self] in
            await self?.runCurrentRep()
        }
    }

    private func runCurrentRep() async {
        guard !Task.isCancelled else { return }

        for tick in stride(from: countdownStart, through: 1, by: -1) {
            phase = .countdown(tick)
            FeedbackManager.countdownTick()
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
        }

        phase = .goFlash
        FeedbackManager.goCue()
        let label = currentGesture?.id ?? "unknown"
        lastCueLabel = label
        lastGoCueSampleID = coordinator.writeMarker(eventType: "go", label: label)

        try? await Task.sleep(for: .milliseconds(300))
        guard !Task.isCancelled else { return }

        phase = .perform
        performProgress = 0
        let performSteps = 25
        for step in 0...performSteps {
            performProgress = Double(step) / Double(performSteps)
            try? await Task.sleep(for: .milliseconds(Int(performDuration * 1000) / performSteps))
            guard !Task.isCancelled else { return }
        }

        phase = .rest
        performProgress = 0
        try? await Task.sleep(for: .seconds(restDuration))
        guard !Task.isCancelled else { return }

        currentRep += 1
        if currentRep >= repsTarget {
            phase = .blockComplete
            coordinator.writeMarker(eventType: "block_end", label: currentGesture?.id ?? "")
            if isLastBlockInCurrentSession {
                coordinator.writeMarker(
                    eventType: "gesture_session_end",
                    label: String(currentGestureSession)
                )
            }
            if settings.showRedonPrompt,
               isLastBlockInCurrentSession,
               currentBlockIndex < blocks.count - 1 {
                phase = .redonPrompt
            } else {
                advanceToNextBlock()
            }
        } else {
            await runCurrentRep()
        }
    }

    private func advanceToNextBlock() {
        currentBlockIndex += 1
        currentRep = 0
        extraRepsForBlock = 0
        if currentBlockIndex >= blocks.count {
            Task { await finishSession() }
        } else {
            phase = .blockIntro
        }
    }

    private var isFirstBlockInCurrentSession: Bool {
        guard currentBlockIndex < blocks.count else { return false }
        return currentBlockIndex == 0
            || blocks[currentBlockIndex - 1].sessionIndex != blocks[currentBlockIndex].sessionIndex
    }

    private var isLastBlockInCurrentSession: Bool {
        guard currentBlockIndex < blocks.count else { return false }
        return currentBlockIndex == blocks.count - 1
            || blocks[currentBlockIndex + 1].sessionIndex != blocks[currentBlockIndex].sessionIndex
    }
}
