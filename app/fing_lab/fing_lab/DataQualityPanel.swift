import SwiftUI

struct DataQualityPanel: View {
    let quality: DataQualityTracker
    let connectionState: BLEConnectionState
    @State private var showFlagDetails = false

    var body: some View {
        // isReceivingSamples/hasStalled are time-based, not state-based, so a
        // periodic tick is needed to notice a stall even when no new samples arrive.
        TimelineView(.periodic(from: .now, by: 0.5)) { _ in
            VStack(alignment: .leading, spacing: 10) {
                if quality.hasStalled {
                    Label("IMU stream stalled — check ring", systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(.red, in: RoundedRectangle(cornerRadius: 8))
                }
                panelBody
            }
        }
    }

    private var panelBody: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label(connectionState.label, systemImage: connectionIcon)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(connectionState.isConnected ? .green : .orange)
                Spacer()
                Text(String(format: "%.1f Hz", quality.effectiveSampleRate))
                    .font(.subheadline.monospacedDigit())
            }

            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Dropped")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("\(quality.droppedSamples) (\(String(format: "%.2f", quality.dropPercentage))%)")
                        .font(.subheadline.monospacedDigit())
                        .foregroundStyle(quality.dropPercentage > 0.5 ? .red : .primary)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text("HW Timestamps")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.1f%%", quality.hardwareTimestampPercentage))
                        .font(.subheadline.monospacedDigit())
                }
            }

            if quality.hasTimestampWarnings {
                Button {
                    showFlagDetails = true
                } label: {
                    Label("Timestamp warnings", systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                }
            }

            Text("\(quality.totalSamples) samples")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        .sheet(isPresented: $showFlagDetails) {
            FlagDetailsSheet(quality: quality)
        }
    }

    private var connectionIcon: String {
        switch connectionState {
        case .connected: "dot.radiowaves.left.and.right"
        case .scanning, .connecting, .reconnecting: "antenna.radiowaves.left.and.right"
        case .poweredOff, .unauthorized: "bolt.slash"
        default: "wifi.slash"
        }
    }
}

private struct FlagDetailsSheet: View {
    let quality: DataQualityTracker
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                if quality.fallbackCount > 0 {
                    LabeledContent("FALLBACK", value: "\(quality.fallbackCount)")
                }
                if quality.fifoOverrunCount > 0 {
                    LabeledContent("FIFO_OVERRUN", value: "\(quality.fifoOverrunCount)")
                }
                if quality.nonmonotonicCount > 0 {
                    LabeledContent("NONMONOTONIC", value: "\(quality.nonmonotonicCount)")
                }
                if quality.interpolatedCount > 0 {
                    LabeledContent("INTERPOLATED", value: "\(quality.interpolatedCount)")
                }
            }
            .navigationTitle("Timestamp Flags")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium])
    }
}
