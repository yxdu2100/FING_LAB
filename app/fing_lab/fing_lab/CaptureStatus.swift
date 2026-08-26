import Foundation

enum CaptureStatusEvent: UInt8, Sendable {
    case waiting = 0
    case gestureDetected = 1
    case tofChecking = 2
    case tofAccepted = 3
    case tofRejected = 4
    case tofError = 5
    case exposureSettling = 6
    case imageReady = 7
    case transferStarted = 8
    case transferComplete = 9
    case transferFailed = 10
    case cameraError = 11
    case cancelled = 12
    case resendAvailable = 13
    case resendStarted = 14
    case awaitingAppAcknowledgement = 15

    var title: String {
        switch self {
        case .waiting: "Waiting for gesture"
        case .gestureDetected: "Gesture detected"
        case .tofChecking: "Checking distance"
        case .tofAccepted: "Distance accepted"
        case .tofRejected: "Distance outside capture range"
        case .tofError: "Distance sensor error"
        case .exposureSettling: "Adjusting exposure"
        case .imageReady: "Image ready"
        case .transferStarted: "Sending image"
        case .transferComplete: "Image sent"
        case .transferFailed: "Image transfer interrupted"
        case .cameraError: "Camera error"
        case .cancelled: "Capture cancelled"
        case .resendAvailable: "Image ready to resend"
        case .resendStarted: "Resending image"
        case .awaitingAppAcknowledgement: "Confirming image save"
        }
    }

    var systemImage: String {
        switch self {
        case .waiting: "hand.raised"
        case .gestureDetected: "hand.tap"
        case .tofChecking: "ruler"
        case .tofAccepted: "checkmark.circle"
        case .tofRejected: "arrow.left.and.right.circle"
        case .tofError, .cameraError: "exclamationmark.triangle"
        case .exposureSettling: "camera.aperture"
        case .imageReady: "photo"
        case .transferStarted, .resendStarted: "arrow.up.circle"
        case .awaitingAppAcknowledgement: "iphone.and.arrow.forward"
        case .transferComplete: "checkmark.circle.fill"
        case .transferFailed: "wifi.exclamationmark"
        case .cancelled: "xmark.circle"
        case .resendAvailable: "arrow.clockwise.circle"
        }
    }

    var isError: Bool {
        switch self {
        case .tofError, .transferFailed, .cameraError: true
        default: false
        }
    }

    var permitsResend: Bool {
        self == .transferFailed || self == .resendAvailable ||
            self == .awaitingAppAcknowledgement
    }
}

struct CaptureStatus: Identifiable, Equatable, Sendable {
    static let packetLength = 16
    static let currentVersion: UInt8 = 1
    static let unknownDistance = UInt16.max

    let event: CaptureStatusEvent
    let attemptID: UInt32
    let deviceTimestampMs: UInt32
    let distanceMm: UInt16?
    let detail: Int16
    let progress: UInt8
    let total: UInt8

    var id: String {
        "\(attemptID)-\(deviceTimestampMs)-\(event.rawValue)-\(progress)"
    }

    init?(data: Data) {
        guard data.count == Self.packetLength,
              data[0] == Self.currentVersion,
              let event = CaptureStatusEvent(rawValue: data[1]) else { return nil }
        self.event = event
        attemptID = data.uint32LE(at: 2)
        deviceTimestampMs = data.uint32LE(at: 6)
        let rawDistance = data.uint16LE(at: 10)
        distanceMm = rawDistance == Self.unknownDistance ? nil : rawDistance
        detail = Int16(bitPattern: data.uint16LE(at: 12))
        progress = data[14]
        total = data[15]
    }

    static let waiting = CaptureStatus(
        event: .waiting,
        attemptID: 0,
        deviceTimestampMs: 0,
        distanceMm: nil,
        detail: 0,
        progress: 0,
        total: 0
    )

    private init(
        event: CaptureStatusEvent,
        attemptID: UInt32,
        deviceTimestampMs: UInt32,
        distanceMm: UInt16?,
        detail: Int16,
        progress: UInt8,
        total: UInt8
    ) {
        self.event = event
        self.attemptID = attemptID
        self.deviceTimestampMs = deviceTimestampMs
        self.distanceMm = distanceMm
        self.detail = detail
        self.progress = progress
        self.total = total
    }
}

private extension Data {
    func uint16LE(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }
}
