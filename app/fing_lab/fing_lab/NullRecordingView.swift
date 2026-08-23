import SwiftUI

struct NullRecordingView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss

    @State private var viewModel: NullRecordingViewModel?
    @State private var showStopConfirm = false

    var body: some View {
        Group {
            if let viewModel {
                content(viewModel)
            } else {
                ProgressView()
            }
        }
        .navigationBarBackButtonHidden(true)
        .onAppear {
            if viewModel == nil {
                let vm = NullRecordingViewModel(coordinator: coordinator, settings: settings)
                viewModel = vm
                vm.start()
            }
        }
        .alert("Stop Recording?", isPresented: $showStopConfirm) {
            Button("Continue", role: .cancel) {}
            Button("Stop & Save") {
                Task {
                    await viewModel?.stop()
                    dismiss()
                }
            }
        }
    }

    @ViewBuilder
    private func content(_ vm: NullRecordingViewModel) -> some View {
        VStack(spacing: 20) {
            DataQualityPanel(
                quality: coordinator.quality,
                connectionState: coordinator.ble.connectionState
            )

            Spacer()

            VStack(spacing: 8) {
                Text("Null Recording")
                    .font(.title2.weight(.semibold))
                Text(DurationFormatterUtil.hms(vm.elapsed))
                    .font(.system(size: 56, weight: .bold, design: .monospaced))
                    .contentTransition(.numericText())
                Text("Screen lock OK · streams in background")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let error = vm.errorMessage {
                Text(error).foregroundStyle(.red)
            }

            Spacer()

            HStack(spacing: 16) {
                Button("Stop & Save") { showStopConfirm = true }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)

                Button("Discard") {
                    vm.cancel()
                    dismiss()
                }
                .foregroundStyle(.red)
            }
        }
        .padding()
        .navigationTitle("Null")
        .navigationBarTitleDisplayMode(.inline)
    }
}
