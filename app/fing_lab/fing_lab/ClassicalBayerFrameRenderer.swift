import CoreGraphics
import CoreImage
import Foundation
import UIKit

/// A deterministic, non-AI reconstruction path for the HM01B0 Bayer frames.
///
/// The original `BayerFrameRenderer` intentionally remains unchanged. This
/// renderer adds black-level subtraction, edge-directed demosaicing, mild
/// color/tone correction, lens-shading compensation, classical denoising,
/// local contrast, and restrained luminance sharpening.
enum ClassicalBayerFrameRenderer {
    private static let blackLevel = 8.0
    private static let whiteLevel = 255.0

    private static let redGain = 1.082
    private static let greenGain = 1.0
    private static let blueGain = 1.692

    // Mild color decorrelation. Row sums are 1.0, preserving neutral tones.
    // These conservative values can later be replaced by a ColorChecker-derived
    // matrix without changing the rest of the pipeline.
    private static let colorMatrix = (
        (1.055, -0.030, -0.025),
        (-0.020, 1.040, -0.020),
        (-0.020, -0.035, 1.055)
    )

    private static let ciContext = CIContext(options: [.cacheIntermediates: false])

    static func makeImage(
        from payload: Data,
        configuration: CameraConfiguration = .current,
        rotation: CaptureImageRotation = CaptureRenderSettings.currentRotation
    ) -> UIImage? {
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

                @inline(__always)
                func linearSample(_ index: Int) -> Double {
                    let value = max(Double(bayer[index]) - blackLevel, 0.0)
                    return min(value / (whiteLevel - blackLevel), 1.0)
                }

                @inline(__always)
                func directionalAverage(
                    negative: Double,
                    positive: Double,
                    perpendicularNegative: Double,
                    perpendicularPositive: Double
                ) -> Double {
                    let preferredGradient = abs(negative - positive)
                    let perpendicularGradient = abs(
                        perpendicularNegative - perpendicularPositive
                    )
                    if preferredGradient < perpendicularGradient {
                        return (negative + positive) * 0.5
                    }
                    if perpendicularGradient < preferredGradient {
                        return (perpendicularNegative + perpendicularPositive) * 0.5
                    }
                    return (negative + positive + perpendicularNegative + perpendicularPositive) * 0.25
                }

                for y in margin..<(rawHeight - margin) {
                    for x in margin..<(rawWidth - margin) {
                        // Match the original renderer's stream orientation and
                        // known sensor-coordinate BGGR parity.
                        let sensorY = rawHeight - 1 - y
                        let index = sensorY * rawWidth + x
                        let outputY = height - 1 - (y - margin)
                        let outputIndex = (outputY * width + x - margin) * 4

                        let center = linearSample(index)
                        let top = linearSample(index - rawWidth)
                        let bottom = linearSample(index + rawWidth)
                        let left = linearSample(index - 1)
                        let right = linearSample(index + 1)
                        let topLeft = linearSample(index - rawWidth - 1)
                        let topRight = linearSample(index - rawWidth + 1)
                        let bottomLeft = linearSample(index + rawWidth - 1)
                        let bottomRight = linearSample(index + rawWidth + 1)

                        let isBlueRow = sensorY.isMultiple(of: 2)
                        let isBlueColumn = x.isMultiple(of: 2)
                        let rgb: (Double, Double, Double)

                        if isBlueRow && isBlueColumn {
                            let green = directionalAverage(
                                negative: left,
                                positive: right,
                                perpendicularNegative: top,
                                perpendicularPositive: bottom
                            )
                            let red = diagonalAverage(
                                topLeft,
                                topRight,
                                bottomLeft,
                                bottomRight
                            )
                            rgb = (red, green, center)
                        } else if isBlueRow {
                            // Green site on a blue row: red is vertical and blue horizontal.
                            rgb = ((top + bottom) * 0.5, center, (left + right) * 0.5)
                        } else if isBlueColumn {
                            // Green site on a red row: red is horizontal and blue vertical.
                            rgb = ((left + right) * 0.5, center, (top + bottom) * 0.5)
                        } else {
                            let green = directionalAverage(
                                negative: left,
                                positive: right,
                                perpendicularNegative: top,
                                perpendicularPositive: bottom
                            )
                            let blue = diagonalAverage(
                                topLeft,
                                topRight,
                                bottomLeft,
                                bottomRight
                            )
                            rgb = (center, green, blue)
                        }

                        let normalizedX = width > 1
                            ? (Double(x - margin) / Double(width - 1)) * 2.0 - 1.0
                            : 0.0
                        let normalizedY = height > 1
                            ? (Double(outputY) / Double(height - 1)) * 2.0 - 1.0
                            : 0.0
                        let cornerNormalizedRadiusSquared = min(
                            (normalizedX * normalizedX + normalizedY * normalizedY) * 0.5,
                            1.0
                        )
                        let lensGain = 1.0 + 0.12 * cornerNormalizedRadiusSquared

                        let balanced = (
                            rgb.0 * redGain * lensGain,
                            rgb.1 * greenGain * lensGain,
                            rgb.2 * blueGain * lensGain
                        )
                        let corrected = applyColorMatrix(balanced)

                        output[outputIndex] = encoded(corrected.0)
                        output[outputIndex + 1] = encoded(corrected.1)
                        output[outputIndex + 2] = encoded(corrected.2)
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

        let extent = CGRect(x: 0, y: 0, width: width, height: height)
        let source = CIImage(cgImage: cgImage)

        // These are traditional spatial filters. Clamping prevents dark edges
        // while filtering, then the image is cropped back to the sensor frame.
        let denoised = source.clampedToExtent().applyingFilter(
            "CINoiseReduction",
            parameters: [
                "inputNoiseLevel": 0.012,
                "inputSharpness": 0.20
            ]
        ).cropped(to: extent)
        let locallyContrasted = denoised.clampedToExtent().applyingFilter(
            "CIUnsharpMask",
            parameters: [
                kCIInputRadiusKey: 7.0,
                kCIInputIntensityKey: 0.25
            ]
        ).cropped(to: extent)
        let sharpened = locallyContrasted.applyingFilter(
            "CISharpenLuminance",
            parameters: [kCIInputSharpnessKey: 0.22]
        )

        guard let processed = ciContext.createCGImage(sharpened, from: extent),
              let rotated = rotatePixels(processed, by: rotation) else { return nil }
        return UIImage(cgImage: rotated)
    }

    @inline(__always)
    private static func diagonalAverage(
        _ topLeft: Double,
        _ topRight: Double,
        _ bottomLeft: Double,
        _ bottomRight: Double
    ) -> Double {
        let descendingGradient = abs(topLeft - bottomRight)
        let ascendingGradient = abs(topRight - bottomLeft)
        if descendingGradient < ascendingGradient {
            return (topLeft + bottomRight) * 0.5
        }
        if ascendingGradient < descendingGradient {
            return (topRight + bottomLeft) * 0.5
        }
        return (topLeft + topRight + bottomLeft + bottomRight) * 0.25
    }

    @inline(__always)
    private static func applyColorMatrix(
        _ rgb: (Double, Double, Double)
    ) -> (Double, Double, Double) {
        let matrix = colorMatrix
        return (
            matrix.0.0 * rgb.0 + matrix.0.1 * rgb.1 + matrix.0.2 * rgb.2,
            matrix.1.0 * rgb.0 + matrix.1.1 * rgb.1 + matrix.1.2 * rgb.2,
            matrix.2.0 * rgb.0 + matrix.2.1 * rgb.1 + matrix.2.2 * rgb.2
        )
    }

    @inline(__always)
    private static func encoded(_ value: Double) -> UInt8 {
        let clipped = min(max(value, 0.0), 1.0)
        let gammaCorrected = pow(clipped, CaptureRenderSettings.classicalGamma)
        return UInt8(clamping: Int((gammaCorrected * 255.0).rounded()))
    }

    /// Materialize rotation into the pixels so previews, PNGs, and exports agree.
    private static func rotatePixels(
        _ image: CGImage,
        by rotation: CaptureImageRotation
    ) -> CGImage? {
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
