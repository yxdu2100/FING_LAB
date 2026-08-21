import Combine
import Foundation
import UIKit

@MainActor
final class CaptureViewModel: ObservableObject {
    @Published private(set) var connectionState: BLECameraTransport.State = .disconnected
    @Published private(set) var latestImage: UIImage?
    @Published private(set) var latestFrameID: UInt32?
    @Published private(set) var captures: [SavedCapture] = []
    @Published var errorMessage: String?
    @Published private(set) var exportArchive: URL?

    private let transport = BLECameraTransport()
    private let store = CaptureStore()

    init() {
        captures = store.load()
        transport.onStateChange = { [weak self] state in
            self?.connectionState = state
        }
        transport.onError = { [weak self] message in
            self?.errorMessage = message
        }
        transport.onFrame = { [weak self] frame in
            self?.receive(frame)
        }
    }

    func reconnect() {
        transport.reconnect()
    }

    func delete(_ capture: SavedCapture) {
        do {
            try store.delete(capture)
            captures.removeAll { $0.id == capture.id }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func deleteAll() {
        do {
            try store.deleteAll()
            captures.removeAll()
            latestImage = nil
            latestFrameID = nil
            exportArchive = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func prepareExport() {
        do {
            exportArchive = try store.makeExportArchive()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func receive(_ frame: ReassembledFrame) {
        guard let image = BayerFrameRenderer.makeImage(from: frame.rawBayer) else {
            errorMessage = "Frame \(frame.header.frameID) could not be reconstructed."
            return
        }

        do {
            let capture = try store.save(frame: frame, image: image)
            latestImage = image
            latestFrameID = frame.header.frameID
            captures.insert(capture, at: 0)
            exportArchive = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
