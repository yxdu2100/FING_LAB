import SwiftUI

struct CaptureStudyView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @State private var confirmDeleteAll = false

    var body: some View {
        ScrollView {
            VStack(spacing: 18) {
                statusCard
                previewCard
                controls
                gallery
            }
            .padding()
        }
        .navigationTitle("Phase 1/2 Capture")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            coordinator.ble.setAutoReconnect(true)
            if !coordinator.ble.connectionState.isConnected {
                coordinator.ble.connect()
            }
            coordinator.ble.enterCaptureMode()
        }
        .onDisappear {
            // Phase 1/2 owns CAPTURE mode. Always release the camera and MCLK
            // when this screen goes away so it cannot leak into Phase 3/4.
            coordinator.ble.enterIdleMode()
        }
        .alert("Capture Error", isPresented: Binding(
            get: { coordinator.captureError != nil },
            set: { if !$0 { coordinator.captureError = nil } }
        )) {
            Button("OK") { coordinator.captureError = nil }
        } message: {
            Text(coordinator.captureError ?? "Unknown error")
        }
        .confirmationDialog(
            "Delete every saved image and raw Bayer file?",
            isPresented: $confirmDeleteAll,
            titleVisibility: .visible
        ) {
            Button("Delete All", role: .destructive) { coordinator.deleteAllCaptures() }
            Button("Cancel", role: .cancel) {}
        }
    }

    private var statusCard: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(coordinator.ble.activeMode == .capture ? Color.green : Color.orange)
                .frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 3) {
                Text(coordinator.ble.connectionState.label).font(.headline)
                Text(coordinator.ble.activeMode == .capture
                     ? "Capture mode ready"
                     : "Preparing capture mode…")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button("Reconnect") {
                coordinator.ble.setAutoReconnect(true)
                coordinator.ble.connect()
                coordinator.ble.enterCaptureMode()
            }
            .buttonStyle(.bordered)
        }
        .padding()
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var previewCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Latest 320 × 240 Preview").font(.headline)
                Spacer()
                if let captureIndex = coordinator.latestCaptureIndex {
                    Text("Frame \(captureIndex)").font(.caption.monospacedDigit())
                }
            }
            ZStack {
                RoundedRectangle(cornerRadius: 12).fill(Color.black)
                if let image = coordinator.latestImage {
                    Image(uiImage: image)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFit()
                } else {
                    ContentUnavailableView(
                        "Waiting for a photo",
                        systemImage: "camera",
                        description: Text("Double flick to run the ToF check and capture.")
                    )
                    .foregroundStyle(.white)
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
        }
    }

    private var controls: some View {
        HStack {
            Text("\(coordinator.captures.count) saved")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Spacer()
            Button("Prepare Export", systemImage: "archivebox") {
                coordinator.prepareCaptureExport()
            }
            .disabled(coordinator.captures.isEmpty)
            if let archive = coordinator.captureExportArchive {
                ShareLink(item: archive) {
                    Label("Share ZIP", systemImage: "square.and.arrow.up")
                }
                .buttonStyle(.borderedProminent)
            }
            Button(role: .destructive) { confirmDeleteAll = true } label: {
                Image(systemName: "trash")
            }
            .disabled(coordinator.captures.isEmpty)
        }
    }

    private var gallery: some View {
        LazyVStack(alignment: .leading, spacing: 12) {
            Text("Saved Captures").font(.title2.bold())
            if coordinator.captures.isEmpty {
                Text("No saved images yet.").foregroundStyle(.secondary)
            }
            ForEach(coordinator.captures) { capture in
                HStack(spacing: 12) {
                    Image(uiImage: capture.image)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFill()
                        .frame(width: 120, height: 90)
                        .clipped()
                        .background(Color.black)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    VStack(alignment: .leading, spacing: 5) {
                        Text("Frame \(capture.captureIndex)").font(.headline)
                        Text("Device frame ID \(capture.frameID)")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                        Text(capture.receivedAt, style: .time)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text("PNG + 324×244 Bayer + JSON")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                        HStack {
                            ShareLink(items: capture.exportURLs) {
                                Label("Export", systemImage: "square.and.arrow.up")
                            }
                            Button(role: .destructive) {
                                coordinator.deleteCapture(capture)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                        .font(.caption)
                    }
                    Spacer()
                }
                .padding(10)
                .background(
                    Color(uiColor: .secondarySystemBackground),
                    in: RoundedRectangle(cornerRadius: 12)
                )
            }
        }
    }
}
