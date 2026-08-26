import SwiftUI

struct ContentView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(AppSettings.self) private var settings
    @State private var showSettings = false

    var body: some View {
        @Bindable var settings = settings

        NavigationStack {
            List {
                Section("Participant") {
                    TextField("Participant ID", text: $settings.participantID)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }

                Section("Ring") {
                    HStack {
                        Circle()
                            .fill(coordinator.ble.connectionState.isConnected ? .green : .orange)
                            .frame(width: 10, height: 10)
                        Text(coordinator.ble.connectionState.label)
                        Spacer()
                        Text(modeLabel)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Button("Connect / Reconnect") {
                        coordinator.ble.setAutoReconnect(true)
                        coordinator.ble.connect()
                    }
                }

                Section("Study") {
                    NavigationLink {
                        CaptureStudyView()
                    } label: {
                        studyRow(
                            title: "Phase 1/2 · Image Capture",
                            subtitle: "Gesture → ToF → photo preview",
                            systemImage: "camera"
                        )
                    }

                    NavigationLink {
                        GuidedSessionView()
                    } label: {
                        studyRow(
                            title: "Phase 3 · Gesture Collection",
                            subtitle: "Manual start for each gesture block",
                            systemImage: "hand.tap"
                        )
                    }
                    .disabled(participantIDIsEmpty)

                    NavigationLink {
                        EverydayTasksView()
                    } label: {
                        studyRow(
                            title: "Phase 4 · Everyday Tasks",
                            subtitle: "Manual start for each two-minute task",
                            systemImage: "figure.walk"
                        )
                    }
                    .disabled(participantIDIsEmpty)
                }

                if participantIDIsEmpty {
                    Section {
                        Text("Enter a participant ID before starting Phase 3 or 4.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }

                Section("Data") {
                    NavigationLink("Saved IMU Sessions") {
                        SessionsListView()
                    }
                    LabeledContent("Saved captures", value: "\(coordinator.captures.count)")
                    LabeledContent("IMU storage") {
                        Text(ByteCountFormatterUtil.string(from: coordinator.sessionStore.totalStorageBytes))
                    }
                }
            }
            .navigationTitle("Fing Lab")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Settings", systemImage: "gear") { showSettings = true }
                }
            }
            .sheet(isPresented: $showSettings) { SettingsView() }
            .onAppear {
                coordinator.sessionStore.refresh()
                coordinator.ble.setAutoReconnect(true)
                if !coordinator.ble.connectionState.isConnected {
                    coordinator.ble.connect()
                }
            }
        }
    }

    private var participantIDIsEmpty: Bool {
        settings.participantID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var modeLabel: String {
        switch coordinator.ble.activeMode {
        case .idle: "Idle"
        case .capture: "Capture"
        case .stream: "IMU Stream"
        }
    }

    private func studyRow(title: String, subtitle: String, systemImage: String) -> some View {
        Label {
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } icon: {
            Image(systemName: systemImage)
        }
        .padding(.vertical, 4)
    }
}
