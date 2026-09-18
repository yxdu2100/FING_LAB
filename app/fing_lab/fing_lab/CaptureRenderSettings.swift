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

enum CaptureRenderPipeline: String, CaseIterable, Identifiable, Codable {
    case original
    case classical

    var id: String { rawValue }

    var label: String {
        switch self {
        case .original: return "Original"
        case .classical: return "Enhanced (non-AI)"
        }
    }
}

enum CaptureRenderSettings {
    static let rotationDefaultsKey = "FingLab.captureImageRotationDegrees"
    static let pipelineDefaultsKey = "FingLab.captureRenderPipeline"
    static let profileVersion = 2
    static let classicalProfileVersion = 1
    static let gamma = 0.92
    static let classicalGamma = 0.82

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

    static var currentPipeline: CaptureRenderPipeline {
        guard let rawValue = UserDefaults.standard.string(forKey: pipelineDefaultsKey) else {
            // Preserve the existing reconstruction unless the participant or
            // researcher explicitly enables the enhanced pipeline.
            return .original
        }
        return CaptureRenderPipeline(rawValue: rawValue) ?? .original
    }

    static func profileVersion(for pipeline: CaptureRenderPipeline) -> Int {
        switch pipeline {
        case .original: return profileVersion
        case .classical: return classicalProfileVersion
        }
    }

    static func gamma(for pipeline: CaptureRenderPipeline) -> Double {
        switch pipeline {
        case .original: return gamma
        case .classical: return classicalGamma
        }
    }
}
