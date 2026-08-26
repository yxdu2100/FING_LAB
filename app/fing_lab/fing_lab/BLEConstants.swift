import CoreBluetooth

enum FirmwareMode: UInt8, Equatable {
    case idle = 0
    case capture = 1
    case stream = 2
}

/// UUIDs and packet sizes shared with firmware/src/ble/ble.c.
enum BLEConstants {
    static let serviceUUID = CBUUID(string: "12345678-9ABC-11EE-BE56-0242AC120002")
    static let imageDataUUID = CBUUID(string: "1234567A-9ABC-11EE-BE56-0242AC120002")
    static let statusUUID = CBUUID(string: "1234567C-9ABC-11EE-BE56-0242AC120002")
    static let imuDataUUID = CBUUID(string: "1234567D-9ABC-11EE-BE56-0242AC120002")
    static let captureStatusUUID = CBUUID(string: "1234567E-9ABC-11EE-BE56-0242AC120002")
    static let commandUUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDE01")

    static let commandIdle = FirmwareMode.idle.rawValue
    static let commandCapture = FirmwareMode.capture.rawValue
    static let commandStream = FirmwareMode.stream.rawValue
    static let commandKeepalive: UInt8 = 3
    static let commandResendLastImage: UInt8 = 4
    static let commandAcknowledgeFrame: UInt8 = 5

    static let samplePayloadLength = 23
    static let maxSamplesPerPacket = 7
    static let inferencePayloadLength = 10
    static let inferencePayloadVersion: UInt8 = 1
    static let deviceName = "FING_LAB"
    static let keepaliveInterval: TimeInterval = 2.0
    static let restoreIdentifier = "com.yuxin.finglab.ble"
}

enum ClassifierKind: UInt8 {
    case mlc = 1
    case cnn = 2
    case hdc = 3
    case unknown = 255

    init(rawValue: UInt8) {
        switch rawValue {
        case Self.mlc.rawValue: self = .mlc
        case Self.cnn.rawValue: self = .cnn
        case Self.hdc.rawValue: self = .hdc
        default: self = .unknown
        }
    }

    var label: String {
        switch self {
        case .mlc: "MLC"
        case .cnn: "CNN"
        case .hdc: "HDC"
        case .unknown: "Unknown"
        }
    }
}

enum TimestampFlags: UInt8, CaseIterable {
    case hardware = 0
    case interpolated = 1
    case fallback = 2
    case fifoOverrun = 3
    case nonmonotonic = 4

    var bit: UInt8 { 1 << rawValue }

    var label: String {
        switch self {
        case .hardware: "HARDWARE"
        case .interpolated: "INTERPOLATED"
        case .fallback: "FALLBACK"
        case .fifoOverrun: "FIFO_OVERRUN"
        case .nonmonotonic: "NONMONOTONIC"
        }
    }

    static func isSet(_ flags: UInt8, _ flag: TimestampFlags) -> Bool {
        (flags & flag.bit) != 0
    }
}
