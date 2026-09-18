import Foundation
import UIKit

/// Chooses a renderer without changing the behavior of the original renderer.
enum CaptureFrameRenderer {
    static func makeImage(
        from payload: Data,
        configuration: CameraConfiguration = .current,
        rotation: CaptureImageRotation = CaptureRenderSettings.currentRotation,
        pipeline: CaptureRenderPipeline = CaptureRenderSettings.currentPipeline
    ) -> UIImage? {
        switch pipeline {
        case .original:
            return BayerFrameRenderer.makeImage(
                from: payload,
                configuration: configuration,
                rotation: rotation
            )
        case .classical:
            return ClassicalBayerFrameRenderer.makeImage(
                from: payload,
                configuration: configuration,
                rotation: rotation
            )
        }
    }
}
