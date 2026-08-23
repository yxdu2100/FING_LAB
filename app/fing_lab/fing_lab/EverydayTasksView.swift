import SwiftUI

struct EverydayTasksView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss

    @State private var viewModel: EverydayTasksViewModel?
    @State private var showCancelConfirm = false

    var body: some View {
        Group {
            if let viewModel {
                content(viewModel)
            } else {
                ProgressView("Starting IMU stream…")
            }
        }
        .navigationBarBackButtonHidden(true)
        .onAppear {
            if viewModel == nil {
                let vm = EverydayTasksViewModel(coordinator: coordinator, settings: settings)
                viewModel = vm
                vm.startSession()
            }
        }
        .alert("Cancel Entire Phase 4 Session?", isPresented: $showCancelConfirm) {
            Button("Keep Recording", role: .cancel) {}
            Button("Discard All Phase 4 Data", role: .destructive) {
                viewModel?.cancel()
                dismiss()
            }
        } message: {
            Text("All IMU data recorded in this session will be deleted.")
        }
    }

    private func content(_ vm: EverydayTasksViewModel) -> some View {
        VStack(spacing: 18) {
            DataQualityPanel(
                quality: coordinator.quality,
                connectionState: coordinator.ble.connectionState
            )

            if let error = vm.errorMessage {
                Text(error).foregroundStyle(.red)
            }

            Spacer()
            phaseContent(vm)
            Spacer()

            if vm.phase != .sessionComplete {
                controlBar(vm)
            }
        }
        .padding()
        .navigationTitle("Phase 4")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func phaseContent(_ vm: EverydayTasksViewModel) -> some View {
        switch vm.phase {
        case .taskIntro:
            if let task = vm.currentTask {
                VStack(spacing: 16) {
                    Text(vm.progressLabel)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(task.name)
                        .font(.largeTitle.bold())
                    Text(task.instruction)
                        .multilineTextAlignment(.center)
                        .foregroundStyle(.secondary)
                    Text("The recording lasts 2:00.")
                        .font(.subheadline.weight(.medium))
                    TimelineView(.periodic(from: .now, by: 0.5)) { _ in
                        if coordinator.quality.isReceivingSamples {
                            Button("Start Task") { vm.startCurrentTask() }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.large)
                        } else {
                            VStack(spacing: 8) {
                                ProgressView()
                                Text("Waiting for ring data…")
                                    .font(.subheadline)
                                    .foregroundStyle(.orange)
                            }
                        }
                    }
                }
            }

        case .countdown(let value):
            VStack(spacing: 8) {
                Text(vm.currentTask?.name ?? "")
                    .font(.title2.bold())
                Text("\(value)")
                    .font(.system(size: 120, weight: .bold, design: .rounded))
                    .contentTransition(.numericText())
            }

        case .recording:
            VStack(spacing: 14) {
                Text(vm.currentTask?.name ?? "Recording")
                    .font(.title.bold())
                Text(duration(vm.secondsRemaining))
                    .font(.system(size: 64, weight: .bold, design: .monospaced))
                    .contentTransition(.numericText())
                Text("Recording IMU data")
                    .foregroundStyle(.green)
            }

        case .paused:
            VStack(spacing: 14) {
                Text(vm.currentTask?.name ?? "Task")
                    .font(.title.bold())
                Text("Paused")
                    .font(.largeTitle.bold())
                Text(duration(vm.secondsRemaining) + " remaining")
                    .font(.title3.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

        case .taskComplete:
            VStack(spacing: 16) {
                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 60))
                    .foregroundStyle(.green)
                Text("Task Complete")
                    .font(.title.bold())
                Button(vm.currentTaskIndex + 1 == vm.tasks.count ? "Finish & Save" : "Next Task") {
                    vm.advance()
                }
                .buttonStyle(.borderedProminent)
                Button("Discard & Redo This Task") { vm.redoCurrentTask() }
                    .buttonStyle(.bordered)
                    .tint(.orange)
            }

        case .sessionComplete:
            VStack(spacing: 16) {
                Image(systemName: "externaldrive.fill.badge.checkmark")
                    .font(.system(size: 60))
                    .foregroundStyle(.green)
                Text("Phase 4 Saved")
                    .font(.title.bold())
                Button("Done") { dismiss() }
                    .buttonStyle(.borderedProminent)
            }
        }
    }

    private func duration(_ seconds: Int) -> String {
        String(format: "%d:%02d", seconds / 60, seconds % 60)
    }

    @ViewBuilder
    private func controlBar(_ vm: EverydayTasksViewModel) -> some View {
        VStack(spacing: 10) {
            switch vm.phase {
            case .countdown, .recording:
                HStack(spacing: 14) {
                    Button("Pause") { vm.pause() }
                        .buttonStyle(.bordered)
                    Button("Discard & Redo Task") { vm.redoCurrentTask() }
                        .buttonStyle(.bordered)
                        .tint(.orange)
                }
            case .paused:
                HStack(spacing: 14) {
                    Button("Resume") { vm.resume() }
                        .buttonStyle(.borderedProminent)
                    Button("Discard & Redo Task") { vm.redoCurrentTask() }
                        .buttonStyle(.bordered)
                        .tint(.orange)
                }
            default:
                EmptyView()
            }

            Button("Cancel Session") { showCancelConfirm = true }
                .foregroundStyle(.red)
        }
    }
}
