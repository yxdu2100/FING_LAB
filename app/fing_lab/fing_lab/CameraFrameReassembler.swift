import Foundation

struct ReassembledFrame {
    let header: CameraFrameHeader
    let rawBayer: Data
}

final class CameraFrameReassembler {
    private enum State {
        case header
        case payload(CameraFrameHeader, Data)
    }

    private let configuration = CameraConfiguration.current
    private var state: State = .header

    func reset() {
        state = .header
    }

    func process(_ packet: Data) -> ReassembledFrame? {
        switch state {
        case .header:
            guard
                packet.count == configuration.headerLength,
                let header = CameraFrameHeader(data: packet),
                header.marker == configuration.marker,
                Int(header.payloadLength) == configuration.payloadLength
            else {
                return nil
            }
            state = .payload(header, Data(capacity: configuration.payloadLength))
            return nil

        case .payload(let header, var buffer):
            buffer.append(packet)
            let expected = Int(header.payloadLength)
            guard buffer.count <= expected else {
                state = .header
                return nil
            }
            guard buffer.count == expected else {
                state = .payload(header, buffer)
                return nil
            }

            state = .header
            return ReassembledFrame(header: header, rawBayer: buffer)
        }
    }
}
