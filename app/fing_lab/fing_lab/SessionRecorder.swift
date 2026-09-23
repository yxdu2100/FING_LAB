import Foundation

final class SessionRecorder {
    private(set) var sessionID: String?
    private(set) var folderURL: URL?
    private(set) var isRecording = false
    private(set) var startDate: Date?

    private var imuFileHandle: FileHandle?
    private var markersFileHandle: FileHandle?
    private var meta: SessionMeta?
    private var lastCheckpointDate: Date?

    private let checkpointInterval: TimeInterval = 2

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
        notes: String = "",
        deviceIdentity: RingDeviceIdentity? = nil
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

        let now = Date()
        startDate = now

        let initialMeta = SessionMeta(
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
            label: mode == .null ? "null" : nil,
            boardID: deviceIdentity?.boardID,
            deviceName: deviceIdentity?.advertisedName,
            ringSizeMm: deviceIdentity?.ringSizeMm,
            peripheralIdentifier: deviceIdentity?.peripheralIdentifier,
            firmwareVersion: deviceIdentity?.firmwareVersion
        )
        try writeMetadata(initialMeta, to: folder)

        imuFileHandle = try FileHandle(forWritingTo: imuURL)
        imuFileHandle?.seekToEndOfFile()

        markersFileHandle = try FileHandle(forWritingTo: markersURL)
        markersFileHandle?.seekToEndOfFile()

        meta = initialMeta
        self.sessionID = sessionID
        folderURL = folder
        lastCheckpointDate = now
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

    /// Keep an active recording recoverable without requiring a final Save
    /// action. CSV data is already appended continuously; this also flushes
    /// it and refreshes meta.json at a bounded interval.
    func checkpointSession(quality: SessionQualitySnapshot) {
        guard isRecording, let folder = folderURL, var meta else { return }

        apply(quality: quality, to: &meta)
        self.meta = meta

        let now = Date()
        guard lastCheckpointDate.map({ now.timeIntervalSince($0) >= checkpointInterval })
                ?? true else { return }

        imuFileHandle?.synchronizeFile()
        markersFileHandle?.synchronizeFile()
        do {
            try writeMetadata(meta, to: folder)
            lastCheckpointDate = now
        } catch {
            // Finalization will surface a persistent storage error to the UI.
            // Until then, keep recording rather than dropping incoming data.
        }
    }

    @discardableResult
    func finalizeSession(quality: SessionQualitySnapshot, notes: String? = nil) throws -> URL? {
        guard isRecording, let folder = folderURL, var meta else { return nil }

        imuFileHandle?.synchronizeFile()
        markersFileHandle?.synchronizeFile()

        meta.endWallclock = ISO8601DateFormatter().string(from: Date())
        apply(quality: quality, to: &meta)
        if let notes { meta.notes = notes }

        try writeMetadata(meta, to: folder)

        imuFileHandle?.closeFile()
        markersFileHandle?.closeFile()
        imuFileHandle = nil
        markersFileHandle = nil

        isRecording = false
        self.meta = nil
        sessionID = nil
        startDate = nil
        lastCheckpointDate = nil

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
        lastCheckpointDate = nil
    }

    private func apply(quality: SessionQualitySnapshot, to meta: inout SessionMeta) {
        meta.totalSamples = quality.totalSamples
        meta.droppedSamples = quality.droppedSamples
        meta.hardwareTimestampCount = quality.hardwareTimestampCount
        meta.interpolatedCount = quality.interpolatedCount
        meta.fallbackCount = quality.fallbackCount
        meta.fifoOverrunCount = quality.fifoOverrunCount
        meta.nonmonotonicCount = quality.nonmonotonicCount
        meta.disconnectCount = quality.disconnectCount
    }

    private func writeMetadata(_ meta: SessionMeta, to folder: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let metaData = try encoder.encode(meta)
        try metaData.write(
            to: folder.appendingPathComponent("meta.json"),
            options: .atomic
        )
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
