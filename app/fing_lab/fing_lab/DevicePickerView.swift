import SwiftUI

struct DevicePickerView: View {
    @Environment(RecordingCoordinator.self) private var coordinator
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                if coordinator.ble.discoveredDevices.isEmpty {
                    ContentUnavailableView(
                        "Looking for Fing rings",
                        systemImage: "dot.radiowaves.left.and.right",
                        description: Text("Keep FING_20MM powered and nearby.")
                    )
                } else {
                    Section("Nearby rings") {
                        ForEach(coordinator.ble.discoveredDevices) { device in
                            Button {
                                if coordinator.ble.selectDevice(device) { dismiss() }
                            } label: {
                                HStack(spacing: 12) {
                                    Image(systemName: isSelected(device) ? "checkmark.circle.fill" : "circle")
                                        .foregroundStyle(isSelected(device) ? .blue : .secondary)
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(device.displayName)
                                            .foregroundStyle(.primary)
                                        HStack(spacing: 8) {
                                            if let size = device.ringSizeLabel { Text(size) }
                                            if device.rssi != 0 { Text("RSSI \(device.rssi) dBm") }
                                            if let version = device.identity?.firmwareVersion {
                                                Text("FW \(version)")
                                            }
                                        }
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                }
                            }
                            .disabled(coordinator.isActiveSession && !isSelected(device))
                        }
                    }
                }

                if coordinator.isActiveSession {
                    Section {
                        Text("Finish or cancel the active Phase 3/4 session before changing rings.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
            }
            .navigationTitle("Choose Ring")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Refresh", systemImage: "arrow.clockwise") {
                        coordinator.ble.startScanning()
                    }
                }
            }
            .onAppear { coordinator.ble.startScanning() }
        }
    }

    private func isSelected(_ device: DiscoveredRingDevice) -> Bool {
        coordinator.ble.selectedDevice?.id == device.id
    }
}

