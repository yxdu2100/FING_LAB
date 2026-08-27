import Foundation

enum RecordingMode: String, Codable, CaseIterable {
    case guided
    case everyday
    case null
}

struct GestureDefinition: Identifiable, Hashable {
    let id: String
    let name: String
    let description: String

    static let defaults: [GestureDefinition] = [
        GestureDefinition(
            id: "double_side_tap",
            name: "Double Side Tap",
            description: "Tap the side of the ring twice quickly with your thumb."
        ),
        GestureDefinition(
            id: "double_pinch",
            name: "Double Pinch",
            description: "Pinch thumb and index finger together twice."
        ),
        GestureDefinition(
            id: "hover",
            name: "Hover",
            description: "Hold your finger above the target without touching it."
        ),
        GestureDefinition(
            id: "double_flick",
            name: "Double Flick",
            description: "Flick your wrist twice in a quick snapping motion."
        ),
    ]
}

struct SessionMarker: Codable {
    let eventType: String
    let label: String
    let cueUnwrappedSampleID: UInt64
    let invalidatedCueUnwrappedSampleID: UInt64?
    let phoneWallclockISO: String
}

struct SessionMeta: Codable {
    var sessionID: String
    var participantID: String
    var mode: String
    var gestureSetVersion: String
    var imuConfig: String
    var startWallclock: String
    var endWallclock: String?
    var totalSamples: UInt64
    var droppedSamples: UInt64
    var hardwareTimestampCount: UInt64
    var interpolatedCount: UInt64
    var fallbackCount: UInt64
    var fifoOverrunCount: UInt64
    var nonmonotonicCount: UInt64
    var disconnectCount: Int
    var notes: String
    var label: String?
    /// Optional fields preserve decoding of sessions recorded before
    /// multi-device support was added.
    var boardID: String?
    var deviceName: String?
    var ringSizeMm: Int?
    var peripheralIdentifier: UUID?
    var firmwareVersion: String?

    var dropPercentage: Double {
        let total = totalSamples + droppedSamples
        guard total > 0 else { return 0 }
        return Double(droppedSamples) / Double(total) * 100
    }

    var hardwareTimestampPercentage: Double {
        guard totalSamples > 0 else { return 0 }
        return Double(hardwareTimestampCount) / Double(totalSamples) * 100
    }
}

struct SessionSummary: Identifiable {
    let id: String
    let folderURL: URL
    let meta: SessionMeta
    let durationSeconds: TimeInterval?
    let folderSizeBytes: Int64

    var displayTitle: String {
        if meta.mode == RecordingMode.everyday.rawValue {
            return "Everyday · \(meta.sessionID)"
        }
        if meta.mode == RecordingMode.null.rawValue {
            return "Null · \(meta.sessionID)"
        }
        return "Guided · \(meta.sessionID)"
    }
}
