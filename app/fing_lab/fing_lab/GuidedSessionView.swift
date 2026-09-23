import SwiftUI

struct GuidedSessionView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss

    @State private var viewModel: GuidedSessionViewModel?
    @State private var showCancelConfirm = false

    var body: some View {
        Group {
            if let viewModel {
                content(viewModel)
            } else {
                ProgressView("Starting session…")
            }
        }
        .navigationBarBackButtonHidden(true)
        .onAppear {
            if viewModel == nil {
                let vm = GuidedSessionViewModel(coordinator: coordinator, settings: settings)
                viewModel = vm
                vm.start()
            }
        }
        .alert("Cancel Session?", isPresented: $showCancelConfirm) {
            Button("Keep Recording", role: .cancel) {}
            Button("Discard", role: .destructive) {
                viewModel?.cancel()
                dismiss()
            }
        } message: {
            Text("Discarded data cannot be recovered.")
        }
    }

    @ViewBuilder
    private func content(_ vm: GuidedSessionViewModel) -> some View {
        VStack(spacing: 16) {
            DataQualityPanel(
                quality: coordinator.quality,
                connectionState: coordinator.ble.connectionState
            )

            if let error = vm.errorMessage {
                Text(error).foregroundStyle(.red)
            }

            if vm.phase != .blockIntro, vm.phase != .sessionComplete, let gesture = vm.currentGesture {
                VStack(spacing: 2) {
                    Text(gesture.name)
                        .font(.headline)
                    Text(vm.blockProgressLabel)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            phaseContent(vm)

            Spacer()

            controlBar(vm)
        }
        .padding()
        .background(goFlashBackground(vm))
        .navigationTitle("Phase 3")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func phaseContent(_ vm: GuidedSessionViewModel) -> some View {
        switch vm.phase {
        case .blockIntro:
            if let gesture = vm.currentGesture {
                VStack(spacing: 16) {
                    Text(vm.blockProgressLabel)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(gesture.name)
                        .font(.largeTitle.bold())
                    Text(gesture.description)
                        .font(.body)
                        .multilineTextAlignment(.center)
                        .foregroundStyle(.secondary)
                    // Gate on samples actually flowing, not just BLE "Connected" - the
                    // command write to start streaming can still be in flight after connect.
                    TimelineView(.periodic(from: .now, by: 0.5)) { _ in
                        if coordinator.quality.isReceivingSamples {
                            Button("Start Block") { vm.startBlock() }
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
            Text("\(value)")
                .font(.system(size: 120, weight: .bold, design: .rounded))
                .contentTransition(.numericText())

        case .goFlash:
            Text("GO")
                .font(.system(size: 100, weight: .heavy, design: .rounded))
                .foregroundStyle(.green)

        case .perform:
            VStack(spacing: 12) {
                Text("Perform")
                    .font(.title.bold())
                ProgressView(value: vm.performProgress)
                    .tint(.green)
            }
            .padding(.horizontal)

        case .rest:
            Text("Rest")
                .font(.title.bold())
                .foregroundStyle(.secondary)

        case .blockComplete, .redonPrompt:
            redonContent(vm)

        case .sessionComplete:
            VStack(spacing: 16) {
                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 64))
                    .foregroundStyle(.green)
                Text("Session Saved Automatically")
                    .font(.title.bold())
                Button("Done") { dismiss() }
                    .buttonStyle(.borderedProminent)
            }

        case .paused:
            Text("Paused")
                .font(.title.bold())
        }
    }

    @ViewBuilder
    private func redonContent(_ vm: GuidedSessionViewModel) -> some View {
        VStack(spacing: 16) {
            Text("Re-don Ring")
                .font(.title.bold())
            Text("Remove and re-don the ring before the next gesture session.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
            Button("Ring Re-donned") { vm.confirmRedon() }
                .buttonStyle(.borderedProminent)
            Button("Skip") { vm.skipRedon() }
        }
    }

    @ViewBuilder
    private func controlBar(_ vm: GuidedSessionViewModel) -> some View {
        if vm.phase != .sessionComplete && vm.phase != .blockIntro
            && vm.phase != .redonPrompt && vm.phase != .blockComplete {
            HStack(spacing: 16) {
                if vm.phase == .paused {
                    Button("Resume") { vm.resume() }
                        .buttonStyle(.borderedProminent)
                } else {
                    Button("Pause") { vm.pause() }
                        .buttonStyle(.bordered)
                }

                Button("Redo Last") { vm.redoLastRep() }
                    .buttonStyle(.bordered)
                    .disabled(vm.lastCueLabel == nil)

                Button("Cancel") { showCancelConfirm = true }
                    .foregroundStyle(.red)
            }
        } else if vm.phase == .blockIntro || vm.phase == .redonPrompt || vm.phase == .blockComplete {
            Button("Cancel Session") { showCancelConfirm = true }
                .foregroundStyle(.red)
        }
    }

    @ViewBuilder
    private func goFlashBackground(_ vm: GuidedSessionViewModel) -> some View {
        if vm.phase == .goFlash {
            Color.green.opacity(0.15).ignoresSafeArea()
        } else {
            Color.clear
        }
    }
}
