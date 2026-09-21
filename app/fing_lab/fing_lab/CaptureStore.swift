import Foundation
import UIKit

struct SavedCapture: Identifiable {
    let id: String
    /// Persistent app-side sequence number. Unlike the device frame ID, this
    /// does not reset when the MCU or app reconnects.
    let captureIndex: UInt64
    let frameID: UInt32
    let deviceTimestampMs: UInt32
    let distanceMm: UInt16?
    let receivedAt: Date
    let imageURL: URL
    let rawURL: URL
    let metadataURL: URL
    let image: UIImage
    let deviceIdentity: RingDeviceIdentity?

    var exportURLs: [URL] { [imageURL, rawURL, metadataURL] }
}

private struct CaptureMetadata: Codable {
    /// Optional so captures saved by older app versions still load.
    let captureIndex: UInt64?
    let frameID: UInt32
    let deviceTimestampMs: UInt32
    /// Optional so captures saved before image-header v2 still load.
    let distanceMm: UInt16?
    let receivedAt: Date
    let rawWidth: Int
    let rawHeight: Int
    var outputWidth: Int
    var outputHeight: Int
    let bayerPattern: String
    let rawFile: String
    let imageFile: String
    let deviceIdentity: RingDeviceIdentity?
    var rotationDegrees: Int?
    var renderPipeline: String?
    var renderGamma: Double?
    var renderProfileVersion: Int?
}

final class CaptureStore {
    private let fileManager = FileManager.default
    private let root: URL
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private var nextCaptureIndex: UInt64 = 0

    private static let nextCaptureIndexKey = "FingLabCaptures.nextCaptureIndex"

    init() {
        let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        root = documents.appendingPathComponent("FingLabCaptures", isDirectory: true)
        encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)

        let existingMaximum = load().map(\.captureIndex).max()
        let inferredNext = existingMaximum.map { $0 &+ 1 } ?? 0
        let persistedNext = (UserDefaults.standard.object(
            forKey: Self.nextCaptureIndexKey
        ) as? NSNumber)?.uint64Value ?? 0
        nextCaptureIndex = max(inferredNext, persistedNext)
        UserDefaults.standard.set(
            NSNumber(value: nextCaptureIndex),
            forKey: Self.nextCaptureIndexKey
        )
    }

    func save(frame: ReassembledFrame, image: UIImage,
              deviceIdentity: RingDeviceIdentity? = nil) throws -> SavedCapture {
        let receivedAt = Date()
        let captureIndex = nextCaptureIndex
        let id = String(format: "capture_%010llu_%lld", captureIndex,
                        Int64(receivedAt.timeIntervalSince1970 * 1_000))
        let imageURL = root.appendingPathComponent(id + ".png")
        let rawURL = root.appendingPathComponent(id + ".bayer")
        let metadataURL = root.appendingPathComponent(id + ".json")
        guard let png = image.pngData() else { throw StoreError.imageEncodingFailed }

        let configuration = CameraConfiguration.current
        let metadata = CaptureMetadata(
            captureIndex: captureIndex,
            frameID: frame.header.frameID,
            deviceTimestampMs: frame.header.timestampMs,
            distanceMm: frame.header.distanceMm,
            receivedAt: receivedAt,
            rawWidth: configuration.rawWidth,
            rawHeight: configuration.rawHeight,
            outputWidth: Int(image.size.width.rounded()),
            outputHeight: Int(image.size.height.rounded()),
            bayerPattern: "BGGR, bottom-left stream origin",
            rawFile: rawURL.lastPathComponent,
            imageFile: imageURL.lastPathComponent,
            deviceIdentity: deviceIdentity,
            rotationDegrees: CaptureRenderSettings.currentRotation.rawValue,
            renderPipeline: CaptureRenderSettings.currentPipeline.rawValue,
            renderGamma: CaptureRenderSettings.gamma(
                for: CaptureRenderSettings.currentPipeline
            ),
            renderProfileVersion: CaptureRenderSettings.profileVersion(
                for: CaptureRenderSettings.currentPipeline
            )
        )

        try png.write(to: imageURL, options: .atomic)
        do {
            try frame.rawBayer.write(to: rawURL, options: .atomic)
            try encoder.encode(metadata).write(to: metadataURL, options: .atomic)
        } catch {
            try? fileManager.removeItem(at: imageURL)
            try? fileManager.removeItem(at: rawURL)
            try? fileManager.removeItem(at: metadataURL)
            throw error
        }

        nextCaptureIndex &+= 1
        UserDefaults.standard.set(
            NSNumber(value: nextCaptureIndex),
            forKey: Self.nextCaptureIndexKey
        )

        return SavedCapture(
            id: id,
            captureIndex: captureIndex,
            frameID: metadata.frameID,
            deviceTimestampMs: metadata.deviceTimestampMs,
            distanceMm: metadata.distanceMm,
            receivedAt: receivedAt,
            imageURL: imageURL,
            rawURL: rawURL,
            metadataURL: metadataURL,
            image: image,
            deviceIdentity: deviceIdentity
        )
    }

    func load() -> [SavedCapture] {
        let metadataURLs = (try? fileManager.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil
        ))?.filter { $0.pathExtension == "json" } ?? []

        let candidates = metadataURLs.compactMap { url -> (URL, CaptureMetadata, UIImage)? in
            guard
                let data = try? Data(contentsOf: url),
                let metadata = try? decoder.decode(CaptureMetadata.self, from: data)
            else { return nil }

            let imageURL = root.appendingPathComponent(metadata.imageFile)
            let rawURL = root.appendingPathComponent(metadata.rawFile)
            guard let image = UIImage(contentsOfFile: imageURL.path),
                  fileManager.fileExists(atPath: rawURL.path) else { return nil }

            return (url, metadata, image)
        }.sorted { $0.1.receivedAt < $1.1.receivedAt }

        // Legacy captures did not store captureIndex. Assign them stable
        // chronological indices while avoiding indices already persisted by
        // newer captures.
        var usedIndices = Set(candidates.compactMap { $0.1.captureIndex })
        var nextLegacyIndex: UInt64 = 0
        let captures = candidates.map { url, metadata, image in
            let captureIndex: UInt64
            if let savedIndex = metadata.captureIndex {
                captureIndex = savedIndex
            } else {
                while usedIndices.contains(nextLegacyIndex) { nextLegacyIndex &+= 1 }
                captureIndex = nextLegacyIndex
                usedIndices.insert(captureIndex)
                nextLegacyIndex &+= 1

                // Persist the inferred value once so deleting another legacy
                // capture later cannot renumber the remaining gallery.
                let migratedMetadata = CaptureMetadata(
                    captureIndex: captureIndex,
                    frameID: metadata.frameID,
                    deviceTimestampMs: metadata.deviceTimestampMs,
                    distanceMm: metadata.distanceMm,
                    receivedAt: metadata.receivedAt,
                    rawWidth: metadata.rawWidth,
                    rawHeight: metadata.rawHeight,
                    outputWidth: metadata.outputWidth,
                    outputHeight: metadata.outputHeight,
                    bayerPattern: metadata.bayerPattern,
                    rawFile: metadata.rawFile,
                    imageFile: metadata.imageFile,
                    deviceIdentity: metadata.deviceIdentity,
                    rotationDegrees: metadata.rotationDegrees,
                    renderPipeline: metadata.renderPipeline,
                    renderGamma: metadata.renderGamma,
                    renderProfileVersion: metadata.renderProfileVersion
                )
                try? encoder.encode(migratedMetadata).write(to: url, options: .atomic)
            }

            let imageURL = root.appendingPathComponent(metadata.imageFile)
            let rawURL = root.appendingPathComponent(metadata.rawFile)
            return SavedCapture(
                id: url.deletingPathExtension().lastPathComponent,
                captureIndex: captureIndex,
                frameID: metadata.frameID,
                deviceTimestampMs: metadata.deviceTimestampMs,
                distanceMm: metadata.distanceMm,
                receivedAt: metadata.receivedAt,
                imageURL: imageURL,
                rawURL: rawURL,
                metadataURL: url,
                image: image,
                deviceIdentity: metadata.deviceIdentity
            )
        }

        return captures.sorted { $0.receivedAt > $1.receivedAt }
    }

    /// Rebuild PNGs from their immutable Bayer files. Re-rendering from raw
    /// avoids cumulative rotation or brightness changes and keeps exports
    /// deterministic when the global orientation changes repeatedly.
    func rerenderAll(rotation: CaptureImageRotation,
                     pipeline: CaptureRenderPipeline = CaptureRenderSettings.currentPipeline,
                     onlyIfNeeded: Bool = false) throws -> [SavedCapture] {
        let metadataURLs = (try? fileManager.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil
        ))?.filter { $0.pathExtension == "json" } ?? []

        for metadataURL in metadataURLs {
            let metadataData = try Data(contentsOf: metadataURL)
            var metadata = try decoder.decode(CaptureMetadata.self, from: metadataData)
            // Captures written before pipeline selection existed used the
            // original renderer, so a missing value migrates to `.original`.
            let savedPipeline = metadata.renderPipeline.flatMap(CaptureRenderPipeline.init)
                ?? .original
            let alreadyCurrent = metadata.rotationDegrees == rotation.rawValue &&
                savedPipeline == pipeline &&
                metadata.renderGamma == CaptureRenderSettings.gamma(for: pipeline) &&
                metadata.renderProfileVersion == CaptureRenderSettings.profileVersion(
                    for: pipeline
                )
            if onlyIfNeeded && alreadyCurrent { continue }

            let rawURL = root.appendingPathComponent(metadata.rawFile)
            let imageURL = root.appendingPathComponent(metadata.imageFile)
            let raw = try Data(contentsOf: rawURL)
            guard let image = CaptureFrameRenderer.makeImage(
                from: raw,
                rotation: rotation,
                pipeline: pipeline
            ),
                  let png = image.pngData() else {
                throw StoreError.imageEncodingFailed
            }

            metadata.rotationDegrees = rotation.rawValue
            metadata.renderPipeline = pipeline.rawValue
            metadata.renderGamma = CaptureRenderSettings.gamma(for: pipeline)
            metadata.renderProfileVersion = CaptureRenderSettings.profileVersion(for: pipeline)
            metadata.outputWidth = Int(image.size.width.rounded())
            metadata.outputHeight = Int(image.size.height.rounded())

            // Keep a rollback copy so a metadata failure cannot leave the PNG
            // and JSON describing different render settings.
            let previousPNG = try? Data(contentsOf: imageURL)
            try png.write(to: imageURL, options: .atomic)
            do {
                try encoder.encode(metadata).write(to: metadataURL, options: .atomic)
            } catch {
                if let previousPNG {
                    try? previousPNG.write(to: imageURL, options: .atomic)
                }
                throw error
            }
        }

        return load()
    }

    func delete(_ capture: SavedCapture) throws {
        for url in capture.exportURLs where fileManager.fileExists(atPath: url.path) {
            try fileManager.removeItem(at: url)
        }
    }

    func deleteAll() throws {
        for url in try fileManager.contentsOfDirectory(at: root,
                                                        includingPropertiesForKeys: nil) {
            try fileManager.removeItem(at: url)
        }
    }

    func makeExportArchive() throws -> URL {
        guard !load().isEmpty else { throw StoreError.noCaptures }
        var coordinatorError: NSError?
        var result: Result<URL, Error>?
        let coordinator = NSFileCoordinator()

        coordinator.coordinate(readingItemAt: root, options: .forUploading,
                               error: &coordinatorError) { coordinatedURL in
            do {
                let destination = fileManager.temporaryDirectory
                    .appendingPathComponent("FingLabCaptures-\(Int(Date().timeIntervalSince1970)).zip")
                if fileManager.fileExists(atPath: destination.path) {
                    try fileManager.removeItem(at: destination)
                }
                try fileManager.copyItem(at: coordinatedURL, to: destination)
                result = .success(destination)
            } catch {
                result = .failure(error)
            }
        }

        if let coordinatorError { throw coordinatorError }
        guard let result else { throw StoreError.exportFailed }
        return try result.get()
    }

    enum StoreError: LocalizedError {
        case imageEncodingFailed
        case noCaptures
        case exportFailed

        var errorDescription: String? {
            switch self {
            case .imageEncodingFailed: return "Could not encode the reconstructed PNG."
            case .noCaptures: return "There are no captures to export."
            case .exportFailed: return "Could not create the export archive."
            }
        }
    }
}
