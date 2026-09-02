import SwiftUI

struct CaptureStudyView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @State private var confirmDeleteAll = false
    @State private var inspectedImage: CaptureImageViewerItem?

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
        .fullScreenCover(item: $inspectedImage) { item in
            CaptureImageViewer(item: item)
        }
    }

    private var statusCard: some View {
        VStack(alignment: .leading, spacing: 14) {
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

            Divider()

            let status = coordinator.ble.latestCaptureStatus
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: status.event.systemImage)
                    .font(.title2)
                    .foregroundStyle(statusColor(status.event))
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 5) {
                    Text(status.event.title)
                        .font(.headline)
                    Text(statusDescription(status))
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    if status.attemptID > 0 {
                        Text("Attempt \(status.attemptID)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.tertiary)
                    }
                }
                Spacer()
                if status.event.permitsResend {
                    Button("Resend") { coordinator.ble.resendLastImage() }
                        .buttonStyle(.borderedProminent)
                        .disabled(coordinator.ble.activeMode != .capture)
                }
            }

            if !coordinator.ble.captureStatusHistory.isEmpty {
                DisclosureGroup("Recent capture events") {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(coordinator.ble.captureStatusHistory.prefix(8)) { event in
                            HStack {
                                Image(systemName: event.event.systemImage)
                                    .foregroundStyle(statusColor(event.event))
                                    .frame(width: 20)
                                Text(event.event.title)
                                Spacer()
                                if let distance = event.distanceMm {
                                    Text("\(distance) mm")
                                        .monospacedDigit()
                                        .foregroundStyle(.secondary)
                                }
                            }
                            .font(.caption)
                        }
                    }
                    .padding(.top, 8)
                }
                .font(.subheadline)
            }
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
                        .contentShape(Rectangle())
                        .onTapGesture {
                            inspectedImage = CaptureImageViewerItem(
                                id: "latest-\(coordinator.latestCaptureIndex ?? 0)",
                                image: image,
                                title: coordinator.latestCaptureIndex.map { "Frame \($0)" }
                                    ?? "Latest Capture"
                            )
                        }
                    VStack {
                        Spacer()
                        HStack {
                            Spacer()
                            Image(systemName: "arrow.up.left.and.arrow.down.right")
                                .padding(9)
                                .background(.ultraThinMaterial, in: Circle())
                                .padding(8)
                        }
                    }
                    .allowsHitTesting(false)
                } else {
                    ContentUnavailableView(
                        "Waiting for a photo",
                        systemImage: "camera",
                        description: Text("Waiting for a gesture to check distance and capture.")
                    )
                    .foregroundStyle(.white)
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
        }
    }

    private func statusDescription(_ status: CaptureStatus) -> String {
        switch status.event {
        case .waiting:
            return "The ring is armed and waiting for a gesture."
        case .gestureDetected:
            return "The ring recognized the gesture."
        case .tofChecking:
            return "Measuring camera-to-surface distance…"
        case .tofAccepted:
            return "\(status.distanceMm ?? 0) mm is within the 60–500 mm range."
        case .tofRejected:
            return "\(status.distanceMm ?? 0) mm is outside the 60–500 mm range; no photo was taken."
        case .tofError:
            return "The ToF measurement failed (firmware error \(status.detail))."
        case .exposureSettling:
            return "Frame \(status.progress) of \(status.total); earlier frames are discarded."
        case .imageReady:
            return "The exposure-settled frame is ready to send."
        case .transferStarted:
            return "Transferring the 324 × 244 Bayer frame over BLE."
        case .transferComplete:
            return "The complete frame was queued to the phone."
        case .transferFailed:
            return "The frame is retained on the ring. Reconnect if needed, then tap Resend."
        case .cameraError:
            let stage = status.total >> 4
            let burstFrame = status.total & 0x0F
            let stageText = cameraStageDescription(stage, row: status.progress)
            return "Capture failed on AE frame \(burstFrame)/4 while \(stageText) (firmware error \(status.detail))."
        case .cancelled:
            return "The capture stopped because the firmware left capture mode."
        case .resendAvailable:
            return "A previous frame was interrupted and is still available."
        case .resendStarted:
            return "Restarting the retained image transfer from the beginning."
        case .awaitingAppAcknowledgement:
            return "The full frame was queued; waiting for the app to confirm it was saved."
        case .cameraDiagnostic:
            let encoded = status.total
            let stage = encoded >> 4
            let burstFrame = encoded & 0x0F
            let stageText = cameraStageDescription(stage, row: status.progress)
            let errorText = status.detail == 0
                ? ""
                : " Last camera result: \(status.detail)."
            return "AE frame \(burstFrame)/4: \(stageText). Firmware time \(status.deviceTimestampMs) ms.\(errorText)"
        }
    }

    private func statusColor(_ event: CaptureStatusEvent) -> Color {
        switch event {
        case .waiting: .secondary
        case .tofRejected, .cancelled, .resendAvailable: .orange
        case .tofError, .transferFailed, .cameraError: .red
        case .cameraDiagnostic: .purple
        case .transferComplete: .green
        default: .blue
        }
    }

    private func cameraStageDescription(_ stage: UInt8, row: UInt8) -> String {
        switch stage {
        case 0: "camera idle"
        case 1: "starting a sensor frame"
        case 2: "searching for vertical blank (probe \(row))"
        case 3: "waiting for sensor row 0"
        case 4: "receiving row \(row) of 244"
        case 5: "recovering the camera stream after a timed-out SPI transaction"
        case 6: "frame acquisition complete"
        case 7: "camera capture returned an error at row \(row)"
        default: "unknown camera stage \(stage), row \(row)"
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
                        .scaledToFit()
                        .frame(width: 120, height: 90)
                        .background(Color.black)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .contentShape(Rectangle())
                        .onTapGesture {
                            inspectedImage = CaptureImageViewerItem(
                                id: capture.id,
                                image: capture.image,
                                title: "Frame \(capture.captureIndex)"
                            )
                        }
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
