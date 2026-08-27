import Foundation

/// Stable identity reported by the firmware and copied into every export.
struct RingDeviceIdentity: Codable, Equatable, Sendable {
    let protocolVersion: Int
    let boardID: String
    let ringSizeMm: Int
    let firmwareVersion: String
    let peripheralIdentifier: UUID
    let advertisedName: String

    var displayName: String { advertisedName.isEmpty ? boardID.uppercased() : advertisedName }
}

/// A nearby Fing Lab board shown in the device picker.
struct DiscoveredRingDevice: Identifiable, Equatable {
    let id: UUID
    var advertisedName: String
    var rssi: Int
    var lastSeen: Date
    var identity: RingDeviceIdentity?

    var displayName: String {
        identity?.displayName ?? (advertisedName.isEmpty ? "FING ring" : advertisedName)
    }

    var ringSizeLabel: String? {
        if let size = identity?.ringSizeMm { return "\(size) mm" }
        let components = advertisedName.split(separator: "_")
        guard let suffix = components.last,
              let value = Int(suffix.replacingOccurrences(of: "MM", with: ""))
        else { return nil }
        return "\(value) mm"
    }
}

