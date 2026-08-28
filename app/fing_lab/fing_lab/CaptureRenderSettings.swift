import Foundation

enum CaptureImageRotation: Int, CaseIterable, Identifiable, Codable {
    case degrees0 = 0
    case degrees90 = 90
    case degrees180 = 180
    case degrees270 = 270

    var id: Int { rawValue }
    var label: String { "\(rawValue)°" }
    var swapsDimensions: Bool { self == .degrees90 || self == .degrees270 }
}

enum CaptureRenderSettings {
    static let rotationDefaultsKey = "FingLab.captureImageRotationDegrees"
    static let profileVersion = 2
    static let gamma = 0.92

    static var currentRotation: CaptureImageRotation {
        let defaults = UserDefaults.standard
        guard defaults.object(forKey: rotationDefaultsKey) != nil else {
            // The present camera mounting is upside down relative to the old
            // reconstruction, so new installations default to 180 degrees.
            return .degrees180
        }
        return CaptureImageRotation(
            rawValue: defaults.integer(forKey: rotationDefaultsKey)
        ) ?? .degrees180
    }
}

