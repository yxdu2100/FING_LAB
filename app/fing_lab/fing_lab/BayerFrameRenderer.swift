import CoreGraphics
import CoreImage
import Foundation
import UIKit

enum BayerFrameRenderer {
    // White-balance gains measured in the existing FingCam reconstruction app.
    private static let redGain = 1.082
    private static let greenGain = 1.0
    private static let blueGain = 1.692
    private static let ciContext = CIContext(options: [.cacheIntermediates: false])

    static func makeImage(from payload: Data,
                          configuration: CameraConfiguration = .current,
                          rotation: CaptureImageRotation = CaptureRenderSettings.currentRotation) -> UIImage? {
        guard payload.count == configuration.payloadLength else { return nil }

        let width = configuration.outputWidth
        let height = configuration.outputHeight
        let margin = configuration.demosaicBorder
        let rawWidth = configuration.rawWidth
        let rawHeight = configuration.rawHeight
        var rgba = Data(count: width * height * 4)

        payload.withUnsafeBytes { inputBytes in
            rgba.withUnsafeMutableBytes { outputBytes in
                guard
                    let bayer = inputBytes.bindMemory(to: UInt8.self).baseAddress,
                    let output = outputBytes.bindMemory(to: UInt8.self).baseAddress
                else { return }

                for y in margin..<(rawHeight - margin) {
                    for x in margin..<(rawWidth - margin) {
                        // The board streams from the bottom-left. Retain the known
                        // sensor-coordinate BGGR parity and flip rows for upright output.
                        let sensorY = rawHeight - 1 - y
                        let index = sensorY * rawWidth + x
                        let outputY = height - 1 - (y - margin)
                        let outputIndex = (outputY * width + x - margin) * 4

                        let center = Int(bayer[index])
                        let top = Int(bayer[index - rawWidth])
                        let bottom = Int(bayer[index + rawWidth])
                        let left = Int(bayer[index - 1])
                        let right = Int(bayer[index + 1])
                        let topLeft = Int(bayer[index - rawWidth - 1])
                        let topRight = Int(bayer[index - rawWidth + 1])
                        let bottomLeft = Int(bayer[index + rawWidth - 1])
                        let bottomRight = Int(bayer[index + rawWidth + 1])

                        let isBlueRow = sensorY.isMultiple(of: 2)
                        let isBlueColumn = x.isMultiple(of: 2)
                        let rgb: (Int, Int, Int)

                        if isBlueRow && isBlueColumn {
                            rgb = ((topLeft + topRight + bottomLeft + bottomRight) / 4,
                                   (top + bottom + left + right) / 4,
                                   center)
                        } else if isBlueRow {
                            rgb = ((top + bottom) / 2, center, (left + right) / 2)
                        } else if isBlueColumn {
                            rgb = ((left + right) / 2, center, (top + bottom) / 2)
                        } else {
                            rgb = (center,
                                   (top + bottom + left + right) / 4,
                                   (topLeft + topRight + bottomLeft + bottomRight) / 4)
                        }

                        output[outputIndex] = corrected(rgb.0, gain: redGain)
                        output[outputIndex + 1] = corrected(rgb.1, gain: greenGain)
                        output[outputIndex + 2] = corrected(rgb.2, gain: blueGain)
                        output[outputIndex + 3] = 255
                    }
                }
            }
        }

        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        let bitmap = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
        guard let cgImage = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: bitmap,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else { return nil }

        guard let rotated = rotatePixels(cgImage, by: rotation) else { return nil }
        return UIImage(cgImage: rotated)
    }

    private static func corrected(_ value: Int, gain: Double) -> UInt8 {
        let normalized = min(max(Double(value) * gain / 255.0, 0.0), 1.0)
        let gammaCorrected = pow(normalized, CaptureRenderSettings.gamma)
        return UInt8(clamping: Int((gammaCorrected * 255.0).rounded()))
    }

    /// Materialize rotation into the PNG pixels instead of using UIImage's
    /// orientation flag. This keeps thumbnails, zoom view, and exports equal.
    private static func rotatePixels(_ image: CGImage,
                                     by rotation: CaptureImageRotation) -> CGImage? {
        guard rotation != .degrees0 else { return image }

        let radians = CGFloat(rotation.rawValue) * .pi / 180.0
        let rotated = CIImage(cgImage: image)
            .transformed(by: CGAffineTransform(rotationAngle: -radians))
        let extent = rotated.extent.integral
        let normalized = rotated.transformed(by: CGAffineTransform(
            translationX: -extent.origin.x,
            y: -extent.origin.y
        ))
        return ciContext.createCGImage(normalized, from: normalized.extent)
    }
}
