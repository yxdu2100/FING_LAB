import Foundation

enum IMUSampleParser {
    static func parseSamples(from data: Data, unwrapper: SampleIDUnwrapper) -> [IMUSample] {
        let sampleLen = BLEConstants.samplePayloadLength
        guard !data.isEmpty, data.count % sampleLen == 0 else { return [] }

        let count = data.count / sampleLen
        var samples: [IMUSample] = []
        samples.reserveCapacity(count)
        let receivedAt = Date()

        for index in 0..<count {
            let offset = index * sampleLen
            let wrappedID = data.readUInt16LE(at: offset)
            let unwrappedID = unwrapper.unwrap(wrappedID)

            samples.append(IMUSample(
                unwrappedSampleID: unwrappedID,
                wrappedSampleID: wrappedID,
                timestampUS: data.readUInt32LE(at: offset + 2),
                timestampTicks: data.readUInt32LE(at: offset + 6),
                timestampFlags: data[offset + 10],
                ax: data.readInt16LE(at: offset + 11),
                ay: data.readInt16LE(at: offset + 13),
                az: data.readInt16LE(at: offset + 15),
                gx: data.readInt16LE(at: offset + 17),
                gy: data.readInt16LE(at: offset + 19),
                gz: data.readInt16LE(at: offset + 21),
                receivedAt: receivedAt
            ))
        }

        return samples
    }
}

enum InferenceResultParser {
    static func parse(from data: Data) -> InferenceResult? {
        guard data.count == BLEConstants.inferencePayloadLength else { return nil }

        let version = data[0]
        guard version == BLEConstants.inferencePayloadVersion else { return nil }

        return InferenceResult(
            version: version,
            classifier: ClassifierKind(rawValue: data[1]),
            classID: data[2],
            rawCode: data[3],
            score: data.readInt16LE(at: 4),
            sampleID: data.readUInt32LE(at: 6),
            receivedAt: Date()
        )
    }
}

final class SampleIDUnwrapper {
    private var epoch: UInt64 = 0
    private var lastWrapped: UInt16?
    private var expectingFirmwareRestart = false

    func unwrap(_ wrapped: UInt16) -> UInt64 {
        if expectingFirmwareRestart {
            // The firmware's own sample_id counter just restarted from 0 (a
            // reconnect re-triggered imu_start_streaming()), not a genuine
            // 16-bit rollover. Rebase the epoch to continue monotonically
            // from the running total instead of using the "wrapped < last"
            // heuristic below, which would otherwise misread this as a wrap
            // and add a bogus +65536 jump (miscounted as ~65536 "dropped"
            // samples downstream).
            epoch += UInt64(lastWrapped ?? 0) + 1
            expectingFirmwareRestart = false
        } else if let last = lastWrapped, wrapped < last {
            epoch += 65_536
        }
        lastWrapped = wrapped
        return epoch + UInt64(wrapped)
    }

    /// Call right before re-sending the start command after a reconnect,
    /// since that causes the firmware to reset its sample_id counter.
    func markFirmwareWillRestart() {
        expectingFirmwareRestart = true
    }

    func reset() {
        epoch = 0
        lastWrapped = nil
        expectingFirmwareRestart = false
    }
}

private extension Data {
    func readUInt16LE(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func readUInt32LE(at offset: Int) -> UInt32 {
        UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }

    func readInt16LE(at offset: Int) -> Int16 {
        Int16(bitPattern: readUInt16LE(at: offset))
    }
}
