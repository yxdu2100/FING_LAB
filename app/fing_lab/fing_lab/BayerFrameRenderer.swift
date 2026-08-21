import CoreGraphics
import Foundation
import UIKit

enum BayerFrameRenderer {
    // White-balance gains measured in the existing FingCam reconstruction app.
    private static let redGain = 1.082
    private static let greenGain = 1.0
    private static let blueGain = 1.692

    static func makeImage(from payload: Data,
                          configuration: CameraConfiguration = .current) -> UIImage? {
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

        return UIImage(cgImage: cgImage)
    }

    private static func corrected(_ value: Int, gain: Double) -> UInt8 {
        UInt8(clamping: Int((Double(value) * gain).rounded()))
    }
}
