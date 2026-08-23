import Foundation

struct IMUSample: Sendable {
    let unwrappedSampleID: UInt64
    let wrappedSampleID: UInt16
    let timestampUS: UInt32
    let timestampTicks: UInt32
    let timestampFlags: UInt8
    let ax: Int16
    let ay: Int16
    let az: Int16
    let gx: Int16
    let gy: Int16
    let gz: Int16
    let receivedAt: Date

    var csvLine: String {
        [
            String(unwrappedSampleID),
            String(timestampUS),
            String(timestampTicks),
            String(timestampFlags),
            String(ax), String(ay), String(az),
            String(gx), String(gy), String(gz),
        ].joined(separator: ",") + "\n"
    }
}

struct InferenceResult: Identifiable, Sendable {
    let id = UUID()
    let version: UInt8
    let classifier: ClassifierKind
    let classID: UInt8
    let rawCode: UInt8
    let score: Int16
    let sampleID: UInt32
    let receivedAt: Date

    var isNull: Bool { classID == 4 }

    var classLabel: String {
        switch classID {
        case 0: "Double Side Tap"
        case 1: "Double Pinch"
        case 2: "Hover"
        case 3: "Double Flick"
        case 4: "Null"
        default: "Unknown"
        }
    }
}

enum BLEConnectionState: Equatable {
    case poweredOff
    case unauthorized
    case idle
    case scanning
    case connecting
    case connected
    case disconnected
    case reconnecting

    var label: String {
        switch self {
        case .poweredOff: "Bluetooth Off"
        case .unauthorized: "Bluetooth Denied"
        case .idle: "Idle"
        case .scanning: "Scanning…"
        case .connecting: "Connecting…"
        case .connected: "Connected"
        case .disconnected: "Disconnected"
        case .reconnecting: "Reconnecting…"
        }
    }

    var isConnected: Bool { self == .connected }
}
