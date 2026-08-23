import Foundation

@Observable
final class DataQualityTracker {
    private(set) var effectiveSampleRate: Double = 0
    private(set) var droppedSamples: UInt64 = 0
    private(set) var totalSamples: UInt64 = 0
    private(set) var hardwareTimestampCount: UInt64 = 0
    private(set) var interpolatedCount: UInt64 = 0
    private(set) var fallbackCount: UInt64 = 0
    private(set) var fifoOverrunCount: UInt64 = 0
    private(set) var nonmonotonicCount: UInt64 = 0
    private(set) var disconnectCount: Int = 0

    private(set) var lastSampleReceivedAt: Date?

    private var lastUnwrappedID: UInt64?
    private var rateWindowStart: Date?
    private var rateWindowStartID: UInt64?
    private let rateWindowDuration: TimeInterval = 2.0
    private let stallThreshold: TimeInterval = 1.0

    /// True once at least one sample has arrived and the stream is currently live.
    /// Used to gate session start so a countdown/cue sequence never runs against a
    /// ring that looks "Connected" over BLE but hasn't actually started streaming.
    var isReceivingSamples: Bool {
        guard let last = lastSampleReceivedAt else { return false }
        return Date().timeIntervalSince(last) < stallThreshold
    }

    /// True if the stream was flowing at some point this session but has gone quiet -
    /// distinct from "never started", which is the expected state before a session begins.
    var hasStalled: Bool {
        totalSamples > 0 && !isReceivingSamples
    }

    var dropPercentage: Double {
        let received = totalSamples + droppedSamples
        guard received > 0 else { return 0 }
        return Double(droppedSamples) / Double(received) * 100
    }

    var hardwareTimestampPercentage: Double {
        guard totalSamples > 0 else { return 0 }
        return Double(hardwareTimestampCount) / Double(totalSamples) * 100
    }

    var hasTimestampWarnings: Bool {
        fallbackCount > 0 || fifoOverrunCount > 0 || nonmonotonicCount > 0
    }

    var flagCounts: [(flag: TimestampFlags, count: UInt64)] {
        [
            (.fallback, fallbackCount),
            (.fifoOverrun, fifoOverrunCount),
            (.nonmonotonic, nonmonotonicCount),
            (.interpolated, interpolatedCount),
        ].filter { $0.count > 0 }
    }

    func reset() {
        effectiveSampleRate = 0
        droppedSamples = 0
        totalSamples = 0
        hardwareTimestampCount = 0
        interpolatedCount = 0
        fallbackCount = 0
        fifoOverrunCount = 0
        nonmonotonicCount = 0
        lastUnwrappedID = nil
        lastSampleReceivedAt = nil
        rateWindowStart = nil
        rateWindowStartID = nil
    }

    func recordDisconnect() {
        disconnectCount += 1
    }

    func ingest(_ samples: [IMUSample]) {
        for sample in samples {
            ingestOne(sample)
        }
    }

    private func ingestOne(_ sample: IMUSample) {
        if let last = lastUnwrappedID {
            let delta = sample.unwrappedSampleID > last
                ? sample.unwrappedSampleID - last
                : 0
            if delta > 1 {
                droppedSamples += delta - 1
            }
        }
        lastUnwrappedID = sample.unwrappedSampleID
        lastSampleReceivedAt = sample.receivedAt
        totalSamples += 1

        let flags = sample.timestampFlags
        if TimestampFlags.isSet(flags, .hardware) { hardwareTimestampCount += 1 }
        if TimestampFlags.isSet(flags, .interpolated) { interpolatedCount += 1 }
        if TimestampFlags.isSet(flags, .fallback) { fallbackCount += 1 }
        if TimestampFlags.isSet(flags, .fifoOverrun) { fifoOverrunCount += 1 }
        if TimestampFlags.isSet(flags, .nonmonotonic) { nonmonotonicCount += 1 }

        updateSampleRate(unwrappedID: sample.unwrappedSampleID, at: sample.receivedAt)
    }

    private func updateSampleRate(unwrappedID: UInt64, at date: Date) {
        if rateWindowStart == nil {
            rateWindowStart = date
            rateWindowStartID = unwrappedID
            return
        }

        guard let start = rateWindowStart, let startID = rateWindowStartID else { return }
        let elapsed = date.timeIntervalSince(start)
        guard elapsed >= rateWindowDuration else { return }

        if unwrappedID > startID {
            effectiveSampleRate = Double(unwrappedID - startID) / elapsed
        }

        rateWindowStart = date
        rateWindowStartID = unwrappedID
    }

    func snapshot() -> SessionQualitySnapshot {
        SessionQualitySnapshot(
            totalSamples: totalSamples,
            droppedSamples: droppedSamples,
            hardwareTimestampCount: hardwareTimestampCount,
            interpolatedCount: interpolatedCount,
            fallbackCount: fallbackCount,
            fifoOverrunCount: fifoOverrunCount,
            nonmonotonicCount: nonmonotonicCount,
            disconnectCount: disconnectCount
        )
    }
}

struct SessionQualitySnapshot: Codable {
    let totalSamples: UInt64
    let droppedSamples: UInt64
    let hardwareTimestampCount: UInt64
    let interpolatedCount: UInt64
    let fallbackCount: UInt64
    let fifoOverrunCount: UInt64
    let nonmonotonicCount: UInt64
    let disconnectCount: Int
}
