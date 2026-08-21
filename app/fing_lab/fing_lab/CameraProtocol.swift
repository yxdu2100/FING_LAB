import CoreBluetooth
import Foundation

enum CameraUUIDs {
    static let service = CBUUID(string: "12345678-9ABC-11EE-BE56-0242AC120002")
    static let image = CBUUID(string: "1234567A-9ABC-11EE-BE56-0242AC120002")
}

struct CameraConfiguration {
    let rawWidth = 324
    let rawHeight = 244
    let demosaicBorder = 2
    let marker: UInt32 = 0x12345678
    let headerLength = 16

    var payloadLength: Int { rawWidth * rawHeight }
    var outputWidth: Int { rawWidth - demosaicBorder * 2 }
    var outputHeight: Int { rawHeight - demosaicBorder * 2 }

    static let current = CameraConfiguration()
}

struct CameraFrameHeader {
    let marker: UInt32
    let frameID: UInt32
    let timestampMs: UInt32
    let payloadLength: UInt32

    init?(data: Data) {
        guard data.count == CameraConfiguration.current.headerLength else { return nil }
        marker = data.uint32LE(at: 0)
        frameID = data.uint32LE(at: 4)
        timestampMs = data.uint32LE(at: 8)
        payloadLength = data.uint32LE(at: 12)
    }
}

extension Data {
    func uint32LE(at offset: Int) -> UInt32 {
        UInt32(self[offset]) |
        (UInt32(self[offset + 1]) << 8) |
        (UInt32(self[offset + 2]) << 16) |
        (UInt32(self[offset + 3]) << 24)
    }
}
