import SwiftUI

struct SessionsListView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @State private var selectedIDs: Set<String> = []
    @State private var isExporting = false
    @State private var exportURL: URL?
    @State private var showShareSheet = false
    @State private var exportError: String?
    @State private var showDeleteConfirm = false

    var body: some View {
        List(selection: $selectedIDs) {
            Section {
                LabeledContent("Total storage") {
                    Text(ByteCountFormatterUtil.string(from: coordinator.sessionStore.totalStorageBytes))
                }
            }

            Section("Sessions") {
                if coordinator.sessionStore.sessions.isEmpty {
                    Text("No sessions yet")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(coordinator.sessionStore.sessions) { session in
                        SessionRow(session: session)
                            .tag(session.id)
                    }
                }
            }
        }
        .navigationTitle("Sessions")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("Export") { exportSelected() }
                    .disabled(selectedIDs.isEmpty || isExporting)
            }
            if !selectedIDs.isEmpty {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Delete", role: .destructive) { showDeleteConfirm = true }
                }
            }
        }
        .environment(\.editMode, .constant(.active))
        .onAppear { coordinator.sessionStore.refresh() }
        .sheet(isPresented: $showShareSheet, onDismiss: {
            if let url = exportURL {
                try? FileManager.default.removeItem(at: url)
                exportURL = nil
            }
        }) {
            if let exportURL {
                ShareSheet(items: [exportURL])
            }
        }
        .alert("Export Failed", isPresented: .constant(exportError != nil)) {
            Button("OK") { exportError = nil }
        } message: {
            Text(exportError ?? "")
        }
        .confirmationDialog(
            "Delete \(selectedIDs.count) selected session(s)?",
            isPresented: $showDeleteConfirm,
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                coordinator.sessionStore.deleteSessions(ids: selectedIDs)
                selectedIDs.removeAll()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Deleted IMU data cannot be recovered.")
        }
        .overlay {
            if isExporting {
                ProgressView("Preparing export…")
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func exportSelected() {
        let folders = coordinator.sessionStore.sessions
            .filter { selectedIDs.contains($0.id) }
            .map(\.folderURL)
        guard !folders.isEmpty else { return }

        isExporting = true
        Task {
            do {
                let url = try ExportService.zipSessions(folders)
                exportURL = url
                showShareSheet = true
            } catch {
                exportError = error.localizedDescription
            }
            isExporting = false
        }
    }
}

private struct SessionRow: View {
    let session: SessionSummary

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(session.displayTitle)
                .font(.headline)
            HStack(spacing: 12) {
                Label("\(session.meta.totalSamples)", systemImage: "waveform")
                Label(String(format: "%.2f%% drop", session.meta.dropPercentage), systemImage: "exclamationmark.triangle")
                    .foregroundStyle(session.meta.dropPercentage > 0.5 ? .red : .secondary)
                Label(String(format: "%.0f%% HW", session.meta.hardwareTimestampPercentage), systemImage: "clock")
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            if let duration = session.durationSeconds {
                Text(DurationFormatterUtil.hms(duration))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.vertical, 4)
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}
