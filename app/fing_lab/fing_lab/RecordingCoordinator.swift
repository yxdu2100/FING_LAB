import Foundation
import SwiftUI
import UIKit

@Observable
final class AppSettings {
    var participantID: String {
        didSet { UserDefaults.standard.set(participantID, forKey: Keys.participantID) }
    }
    var repsPerBlock: Int {
        didSet { UserDefaults.standard.set(repsPerBlock, forKey: Keys.repsPerBlock) }
    }
    var sessionsPerGesture: Int {
        didSet { UserDefaults.standard.set(sessionsPerGesture, forKey: Keys.sessionsPerGesture) }
    }
    var gestureSetVersion: String {
        didSet { UserDefaults.standard.set(gestureSetVersion, forKey: Keys.gestureSetVersion) }
    }
    var imuConfig: String {
        didSet { UserDefaults.standard.set(imuConfig, forKey: Keys.imuConfig) }
    }
    var showRedonPrompt: Bool {
        didSet { UserDefaults.standard.set(showRedonPrompt, forKey: Keys.showRedonPrompt) }
    }
    var fastGuidedMode: Bool {
        didSet { UserDefaults.standard.set(fastGuidedMode, forKey: Keys.fastGuidedMode) }
    }
    var enabledGestureIDs: [String] {
        didSet { UserDefaults.standard.set(enabledGestureIDs, forKey: Keys.enabledGestureIDs) }
    }

    var enabledGestures: [GestureDefinition] {
        let defaults = GestureDefinition.defaults
        if enabledGestureIDs.isEmpty { return defaults }
        return enabledGestureIDs.compactMap { id in defaults.first { $0.id == id } }
    }

    private enum Keys {
        static let participantID = "participantID"
        static let repsPerBlock = "repsPerBlock"
        static let sessionsPerGesture = "sessionsPerGesture"
        static let phase3StructureVersion = "phase3StructureVersion"
        static let gestureSetVersion = "gestureSetVersion"
        static let imuConfig = "imuConfig"
        static let showRedonPrompt = "showRedonPrompt"
        static let fastGuidedMode = "fastGuidedMode"
        static let enabledGestureIDs = "enabledGestureIDs"
    }

    init() {
        let defaults = UserDefaults.standard
        participantID = defaults.string(forKey: Keys.participantID) ?? ""
        if defaults.integer(forKey: Keys.phase3StructureVersion) < 2 {
            // Migrate the original 4 × 15 configuration to the study protocol:
            // 4 gestures × 2 sessions × 8 repetitions.
            repsPerBlock = 8
            sessionsPerGesture = 2
            defaults.set(8, forKey: Keys.repsPerBlock)
            defaults.set(2, forKey: Keys.sessionsPerGesture)
            defaults.set(2, forKey: Keys.phase3StructureVersion)
        } else {
            repsPerBlock = max(1, defaults.integer(forKey: Keys.repsPerBlock).nonZeroOr(8))
            sessionsPerGesture = max(
                1,
                defaults.integer(forKey: Keys.sessionsPerGesture).nonZeroOr(2)
            )
        }
        gestureSetVersion = defaults.string(forKey: Keys.gestureSetVersion) ?? "v1"
        imuConfig = defaults.string(forKey: Keys.imuConfig) ?? "120hz_8g_2000dps"
        showRedonPrompt = defaults.object(forKey: Keys.showRedonPrompt) as? Bool ?? true
        fastGuidedMode = defaults.object(forKey: Keys.fastGuidedMode) as? Bool ?? false
        let savedGestureIDs = defaults.stringArray(forKey: Keys.enabledGestureIDs)
            ?? GestureDefinition.defaults.map(\.id)
        // Migrate installations of RingCollector that stored the old label.
        enabledGestureIDs = savedGestureIDs.map { $0 == "pinch_hold" ? "hover" : $0 }
    }
}

private extension Int {
    func nonZeroOr(_ fallback: Int) -> Int { self == 0 ? fallback : self }
}

@Observable
final class RecordingCoordinator {
    let ble = RingBLEManager()
    let recorder = SessionRecorder()
    let quality = DataQualityTracker()
    let sessionStore = SessionStore()
    let captureStore = CaptureStore()

    var isActiveSession = false
    var latestImage: UIImage?
    var latestCaptureIndex: UInt64?
    var captures: [SavedCapture] = []
    var captureError: String?
    var captureExportArchive: URL?

    init() {
        ble.onSamplesReceived = { [weak self] samples in
            guard let self else { return }
            self.quality.ingest(samples)
            self.recorder.appendSamples(samples)
        }

        ble.onConnectionEvent = { [weak self] event, sampleID in
            guard let self, self.isActiveSession else { return }
            let cueID = sampleID ?? self.ble.lastUnwrappedSampleID ?? 0
            self.recorder.appendMarker(eventType: event, label: event, cueUnwrappedSampleID: cueID)
            if event == "disconnect" {
                self.quality.recordDisconnect()
            }
        }

        captures = captureStore.load()
        latestImage = captures.first?.image
        latestCaptureIndex = captures.first?.captureIndex
        ble.onFrameReceived = { [weak self] frame in
            guard let self else { return }
            guard let image = BayerFrameRenderer.makeImage(from: frame.rawBayer) else {
                self.captureError = "Could not reconstruct the 324 × 244 Bayer frame."
                return
            }
            do {
                let saved = try self.captureStore.save(frame: frame, image: image)
                self.captures.insert(saved, at: 0)
                self.latestImage = image
                self.latestCaptureIndex = saved.captureIndex
                self.captureExportArchive = nil
            } catch {
                self.captureError = error.localizedDescription
            }
        }
    }

    func beginSession(
        participantID: String,
        mode: RecordingMode,
        gestureSetVersion: String = "v1",
        imuConfig: String = "120hz_8g_2000dps",
        notes: String = ""
    ) throws {
        ble.resetForNewSession()
        quality.reset()
        try recorder.startSession(
            participantID: participantID,
            mode: mode,
            gestureSetVersion: gestureSetVersion,
            imuConfig: imuConfig,
            notes: notes
        )
        isActiveSession = true
        ble.setAutoReconnect(true)
        if !ble.connectionState.isConnected {
            ble.connect()
        }
        ble.startStreaming()
    }

    func endSession(notes: String? = nil) throws {
        ble.stopStreaming()
        _ = try recorder.finalizeSession(quality: quality.snapshot(), notes: notes)
        isActiveSession = false
        sessionStore.refresh()
    }

    func cancelSession() {
        ble.stopStreaming()
        recorder.cancelSession()
        isActiveSession = false
    }

    func deleteCapture(_ capture: SavedCapture) {
        do {
            try captureStore.delete(capture)
            captures.removeAll { $0.id == capture.id }
            latestImage = captures.first?.image
            latestCaptureIndex = captures.first?.captureIndex
            captureExportArchive = nil
        } catch {
            captureError = error.localizedDescription
        }
    }

    func deleteAllCaptures() {
        do {
            try captureStore.deleteAll()
            captures = []
            latestImage = nil
            latestCaptureIndex = nil
            captureExportArchive = nil
        } catch {
            captureError = error.localizedDescription
        }
    }

    func prepareCaptureExport() {
        do {
            captureExportArchive = try captureStore.makeExportArchive()
        } catch {
            captureError = error.localizedDescription
        }
    }

    @discardableResult
    func writeMarker(
        eventType: String,
        label: String,
        invalidatedCueUnwrappedSampleID: UInt64? = nil
    ) -> UInt64 {
        let cueID = ble.lastUnwrappedSampleID ?? 0
        recorder.appendMarker(
            eventType: eventType,
            label: label,
            cueUnwrappedSampleID: cueID,
            invalidatedCueUnwrappedSampleID: invalidatedCueUnwrappedSampleID
        )
        return cueID
    }
}
