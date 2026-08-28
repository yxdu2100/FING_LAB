import SwiftUI

struct SettingsView: View {
    @Environment(AppSettings.self) private var settings
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var settings = settings

        NavigationStack {
            Form {
                Section("Study") {
                    TextField("Participant ID", text: $settings.participantID)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Stepper(
                        "Repetitions per block: \(settings.repsPerBlock)",
                        value: $settings.repsPerBlock,
                        in: 1...50
                    )
                    Stepper(
                        "Sessions per gesture: \(settings.sessionsPerGesture)",
                        value: $settings.sessionsPerGesture,
                        in: 1...5
                    )
                    Text("With \(settings.enabledGestures.count) enabled gestures, Phase 3 will contain \(settings.enabledGestures.count * settings.sessionsPerGesture) blocks.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    TextField("Gesture set version", text: $settings.gestureSetVersion)
                    TextField("IMU config", text: $settings.imuConfig)
                }

                Section("Gestures") {
                    ForEach(GestureDefinition.defaults) { gesture in
                        Toggle(isOn: gestureBinding(gesture.id)) {
                            VStack(alignment: .leading) {
                                Text(gesture.name)
                                Text(gesture.description)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }

                Section("Guided Mode") {
                    Toggle("Fast mode", isOn: $settings.fastGuidedMode)
                    Text("Uses a 1-second countdown and 0.5-second rest between guided reps.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Toggle("Re-don ring between sessions", isOn: $settings.showRedonPrompt)
                }

                Section("Captured Images") {
                    Picker("Image rotation", selection: rotationBinding) {
                        ForEach(CaptureImageRotation.allCases) { rotation in
                            Text(rotation.label).tag(rotation)
                        }
                    }
                    if coordinator.isReprocessingCaptures {
                        ProgressView("Updating saved PNG files…")
                    }
                    Text("The selected rotation is applied to the current preview, every saved PNG, and all future captures. Raw Bayer files are never modified.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section("BLE Protocol") {
                    LabeledContent("Service", value: "…120002")
                    LabeledContent("Mode control", value: "0=idle, 1=capture, 2=IMU")
                    LabeledContent("Keepalive", value: "Command 3 every 2s")
                    Text("Image and IMU notifications use separate characteristics and are accepted only in their matching firmware mode.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Settings")
            .alert("Image Processing", isPresented: Binding(
                get: { coordinator.captureError != nil },
                set: { if !$0 { coordinator.captureError = nil } }
            )) {
                Button("OK") { coordinator.captureError = nil }
            } message: {
                Text(coordinator.captureError ?? "Unknown image processing error")
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private var rotationBinding: Binding<CaptureImageRotation> {
        Binding(
            get: { settings.captureImageRotation },
            set: { rotation in
                guard rotation != settings.captureImageRotation else { return }
                if coordinator.applyCaptureRotation(rotation) {
                    settings.captureImageRotation = rotation
                }
            }
        )
    }

    private func gestureBinding(_ id: String) -> Binding<Bool> {
        Binding(
            get: { settings.enabledGestureIDs.contains(id) },
            set: { enabled in
                if enabled {
                    if !settings.enabledGestureIDs.contains(id) {
                        settings.enabledGestureIDs.append(id)
                    }
                } else {
                    settings.enabledGestureIDs.removeAll { $0 == id }
                }
            }
        )
    }
}
