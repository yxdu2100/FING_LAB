import Foundation

final class SessionRecorder {
    private(set) var sessionID: String?
    private(set) var folderURL: URL?
    private(set) var isRecording = false
    private(set) var startDate: Date?

    private var imuFileHandle: FileHandle?
    private var markersFileHandle: FileHandle?
    private var meta: SessionMeta?

    private let fileManager = FileManager.default
    private let sessionsRoot: URL

    init() {
        let docs = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        sessionsRoot = docs.appendingPathComponent("Sessions", isDirectory: true)
        try? fileManager.createDirectory(at: sessionsRoot, withIntermediateDirectories: true)
    }

    func startSession(
        participantID: String,
        mode: RecordingMode,
        gestureSetVersion: String = "v1",
        imuConfig: String = "120hz_8g_2000dps",
        notes: String = ""
    ) throws {
        guard !isRecording else { return }

        let sessionID = try nextSessionID()
        let folder = sessionsRoot.appendingPathComponent(sessionID, isDirectory: true)
        try fileManager.createDirectory(at: folder, withIntermediateDirectories: true)

        let imuURL = folder.appendingPathComponent("imu.csv")
        let markersURL = folder.appendingPathComponent("markers.csv")

        let imuHeader = "unwrapped_sample_id,timestamp_us,timestamp_ticks,timestamp_flags,ax,ay,az,gx,gy,gz\n"
        let markersHeader = "event_type,label,cue_unwrapped_sample_id,invalidated_cue_unwrapped_sample_id,phone_wallclock_iso\n"

        try imuHeader.write(to: imuURL, atomically: true, encoding: .utf8)
        try markersHeader.write(to: markersURL, atomically: true, encoding: .utf8)

        imuFileHandle = try FileHandle(forWritingTo: imuURL)
        imuFileHandle?.seekToEndOfFile()

        markersFileHandle = try FileHandle(forWritingTo: markersURL)
        markersFileHandle?.seekToEndOfFile()

        let now = Date()
        startDate = now

        meta = SessionMeta(
            sessionID: sessionID,
            participantID: participantID,
            mode: mode.rawValue,
            gestureSetVersion: gestureSetVersion,
            imuConfig: imuConfig,
            startWallclock: ISO8601DateFormatter().string(from: now),
            endWallclock: nil,
            totalSamples: 0,
            droppedSamples: 0,
            hardwareTimestampCount: 0,
            interpolatedCount: 0,
            fallbackCount: 0,
            fifoOverrunCount: 0,
            nonmonotonicCount: 0,
            disconnectCount: 0,
            notes: notes,
            label: mode == .null ? "null" : nil
        )

        self.sessionID = sessionID
        folderURL = folder
        isRecording = true
    }

    func appendSamples(_ samples: [IMUSample]) {
        guard isRecording, let handle = imuFileHandle else { return }
        let data = samples.map(\.csvLine).joined().data(using: .utf8) ?? Data()
        handle.write(data)
    }

    func appendMarker(
        eventType: String,
        label: String,
        cueUnwrappedSampleID: UInt64,
        invalidatedCueUnwrappedSampleID: UInt64? = nil
    ) {
        guard isRecording, let handle = markersFileHandle else { return }
        let iso = ISO8601DateFormatter().string(from: Date())
        let invalidatedCue = invalidatedCueUnwrappedSampleID.map { String($0) } ?? ""
        let line = "\(eventType),\(label),\(cueUnwrappedSampleID),\(invalidatedCue),\(iso)\n"
        handle.write(line.data(using: .utf8) ?? Data())
    }

    @discardableResult
    func finalizeSession(quality: SessionQualitySnapshot, notes: String? = nil) throws -> URL? {
        guard isRecording, let folder = folderURL, var meta else { return nil }

        imuFileHandle?.synchronizeFile()
        markersFileHandle?.synchronizeFile()
        imuFileHandle?.closeFile()
        markersFileHandle?.closeFile()
        imuFileHandle = nil
        markersFileHandle = nil

        meta.endWallclock = ISO8601DateFormatter().string(from: Date())
        meta.totalSamples = quality.totalSamples
        meta.droppedSamples = quality.droppedSamples
        meta.hardwareTimestampCount = quality.hardwareTimestampCount
        meta.interpolatedCount = quality.interpolatedCount
        meta.fallbackCount = quality.fallbackCount
        meta.fifoOverrunCount = quality.fifoOverrunCount
        meta.nonmonotonicCount = quality.nonmonotonicCount
        meta.disconnectCount = quality.disconnectCount
        if let notes { meta.notes = notes }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let metaData = try encoder.encode(meta)
        try metaData.write(to: folder.appendingPathComponent("meta.json"))

        isRecording = false
        self.meta = nil
        sessionID = nil
        startDate = nil

        return folder
    }

    func cancelSession() {
        imuFileHandle?.closeFile()
        markersFileHandle?.closeFile()
        imuFileHandle = nil
        markersFileHandle = nil

        if let folder = folderURL {
            try? fileManager.removeItem(at: folder)
        }

        isRecording = false
        meta = nil
        sessionID = nil
        folderURL = nil
        startDate = nil
    }

    private func nextSessionID() throws -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd"
        let datePrefix = formatter.string(from: Date())

        let existing = try fileManager.contentsOfDirectory(at: sessionsRoot, includingPropertiesForKeys: nil)
            .map { $0.lastPathComponent }
            .filter { $0.hasPrefix(datePrefix) }

        let indices = existing.compactMap { name -> Int? in
            let parts = name.split(separator: "_")
            guard parts.count == 2, let index = Int(parts[1]) else { return nil }
            return index
        }

        let nextIndex = (indices.max() ?? 0) + 1
        return String(format: "%@_%03d", datePrefix, nextIndex)
    }
}
