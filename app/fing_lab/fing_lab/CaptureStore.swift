import Foundation
import UIKit

struct SavedCapture: Identifiable {
    let id: String
    let frameID: UInt32
    let deviceTimestampMs: UInt32
    let receivedAt: Date
    let imageURL: URL
    let rawURL: URL
    let metadataURL: URL
    let image: UIImage

    var exportURLs: [URL] { [imageURL, rawURL, metadataURL] }
}

private struct CaptureMetadata: Codable {
    let frameID: UInt32
    let deviceTimestampMs: UInt32
    let receivedAt: Date
    let rawWidth: Int
    let rawHeight: Int
    let outputWidth: Int
    let outputHeight: Int
    let bayerPattern: String
    let rawFile: String
    let imageFile: String
}

final class CaptureStore {
    private let fileManager = FileManager.default
    private let root: URL
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    init() {
        let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        root = documents.appendingPathComponent("FingLabCaptures", isDirectory: true)
        encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)
    }

    func save(frame: ReassembledFrame, image: UIImage) throws -> SavedCapture {
        let receivedAt = Date()
        let id = String(format: "frame_%010u_%lld", frame.header.frameID,
                        Int64(receivedAt.timeIntervalSince1970 * 1_000))
        let imageURL = root.appendingPathComponent(id + ".png")
        let rawURL = root.appendingPathComponent(id + ".bayer")
        let metadataURL = root.appendingPathComponent(id + ".json")
        guard let png = image.pngData() else { throw StoreError.imageEncodingFailed }

        let configuration = CameraConfiguration.current
        let metadata = CaptureMetadata(
            frameID: frame.header.frameID,
            deviceTimestampMs: frame.header.timestampMs,
            receivedAt: receivedAt,
            rawWidth: configuration.rawWidth,
            rawHeight: configuration.rawHeight,
            outputWidth: configuration.outputWidth,
            outputHeight: configuration.outputHeight,
            bayerPattern: "BGGR, bottom-left stream origin",
            rawFile: rawURL.lastPathComponent,
            imageFile: imageURL.lastPathComponent
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

        return SavedCapture(id: id, frameID: metadata.frameID,
                            deviceTimestampMs: metadata.deviceTimestampMs,
                            receivedAt: receivedAt, imageURL: imageURL,
                            rawURL: rawURL, metadataURL: metadataURL, image: image)
    }

    func load() -> [SavedCapture] {
        let metadataURLs = (try? fileManager.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil
        ))?.filter { $0.pathExtension == "json" } ?? []

        return metadataURLs.compactMap { url in
            guard
                let data = try? Data(contentsOf: url),
                let metadata = try? decoder.decode(CaptureMetadata.self, from: data)
            else { return nil }

            let imageURL = root.appendingPathComponent(metadata.imageFile)
            let rawURL = root.appendingPathComponent(metadata.rawFile)
            guard let image = UIImage(contentsOfFile: imageURL.path),
                  fileManager.fileExists(atPath: rawURL.path) else { return nil }
            return SavedCapture(
                id: url.deletingPathExtension().lastPathComponent,
                frameID: metadata.frameID,
                deviceTimestampMs: metadata.deviceTimestampMs,
                receivedAt: metadata.receivedAt,
                imageURL: imageURL,
                rawURL: rawURL,
                metadataURL: url,
                image: image
            )
        }.sorted { $0.receivedAt > $1.receivedAt }
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
