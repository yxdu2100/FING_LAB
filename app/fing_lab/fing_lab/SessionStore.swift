import Foundation

@Observable
final class SessionStore {
    private(set) var sessions: [SessionSummary] = []
    private(set) var totalStorageBytes: Int64 = 0
    private(set) var totalNullMinutes: Double = 0

    private let fileManager = FileManager.default
    private var sessionsRoot: URL {
        let docs = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Sessions", isDirectory: true)
    }

    func refresh() {
        guard let folders = try? fileManager.contentsOfDirectory(
            at: sessionsRoot,
            includingPropertiesForKeys: [.fileSizeKey, .isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            sessions = []
            totalStorageBytes = 0
            totalNullMinutes = 0
            return
        }

        var loaded: [SessionSummary] = []
        var storage: Int64 = 0
        var nullSeconds: TimeInterval = 0

        for folder in folders {
            var isDir: ObjCBool = false
            guard fileManager.fileExists(atPath: folder.path, isDirectory: &isDir), isDir.boolValue else { continue }

            let metaURL = folder.appendingPathComponent("meta.json")
            guard let data = try? Data(contentsOf: metaURL),
                  let meta = try? JSONDecoder().decode(SessionMeta.self, from: data) else { continue }

            let size = directorySize(folder)
            storage += size

            let duration = durationForSession(meta: meta)
            if meta.mode == RecordingMode.null.rawValue, let duration {
                nullSeconds += duration
            }

            loaded.append(SessionSummary(
                id: meta.sessionID,
                folderURL: folder,
                meta: meta,
                durationSeconds: duration,
                folderSizeBytes: size
            ))
        }

        sessions = loaded.sorted { $0.meta.startWallclock > $1.meta.startWallclock }
        totalStorageBytes = storage
        totalNullMinutes = nullSeconds / 60.0
    }

    func deleteSessions(ids: Set<String>) {
        for session in sessions where ids.contains(session.id) {
            try? fileManager.removeItem(at: session.folderURL)
        }
        refresh()
    }

    private func directorySize(_ url: URL) -> Int64 {
        guard let enumerator = fileManager.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        var total: Int64 = 0
        for case let fileURL as URL in enumerator {
            let size = (try? fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            total += Int64(size)
        }
        return total
    }

    private func durationForSession(meta: SessionMeta) -> TimeInterval? {
        let formatter = ISO8601DateFormatter()
        guard let start = formatter.date(from: meta.startWallclock),
              let endString = meta.endWallclock,
              let end = formatter.date(from: endString) else { return nil }
        return end.timeIntervalSince(start)
    }
}

enum ExportService {
    static func zipSessions(_ folders: [URL]) throws -> URL {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("FingLabExport_\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)

        for folder in folders {
            let dest = tempDir.appendingPathComponent(folder.lastPathComponent, isDirectory: true)
            try FileManager.default.copyItem(at: folder, to: dest)
        }

        let zipURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("FingLab_\(ISO8601DateFormatter().string(from: Date())).zip")

        if FileManager.default.fileExists(atPath: zipURL.path) {
            try FileManager.default.removeItem(at: zipURL)
        }

        var coordinatorError: NSError?
        var resultURL: URL?
        var copyError: Error?
        let coordinator = NSFileCoordinator()
        coordinator.coordinate(readingItemAt: tempDir, options: .forUploading, error: &coordinatorError) { zipTemporaryURL in
            do {
                try FileManager.default.copyItem(at: zipTemporaryURL, to: zipURL)
                resultURL = zipURL
            } catch {
                copyError = error
            }
        }

        if let coordinatorError { throw coordinatorError }
        if let copyError { throw copyError }
        guard let resultURL else { throw ExportError.zipFailed }
        return resultURL
    }
}

enum ExportError: LocalizedError {
    case zipFailed

    var errorDescription: String? {
        switch self {
        case .zipFailed: "Failed to create export archive."
        }
    }
}
