import CoreBluetooth
import Foundation

@Observable
final class RingBLEManager: NSObject {
    var connectionState: BLEConnectionState = .idle
    var activeMode: FirmwareMode = .idle
    var isStreaming = false
    var lastUnwrappedSampleID: UInt64?
    var connectedPeripheralName: String?
    var errorMessage: String?
    var latestCaptureStatus: CaptureStatus = .waiting
    var captureStatusHistory: [CaptureStatus] = []
    var isCameraRecoveryInProgress = false
    var discoveredDevices: [DiscoveredRingDevice] = []
    var connectedDeviceIdentity: RingDeviceIdentity?

    var onSamplesReceived: (([IMUSample]) -> Void)?
    var onConnectionEvent: ((String, UInt64?) -> Void)?
    var onFrameReceived: ((ReassembledFrame) -> Void)?
    var onCaptureStatusReceived: ((CaptureStatus) -> Void)?
    /// The coordinator uses this to prohibit changing hardware in the middle
    /// of a Phase 3/4 recording, including while that recording is paused.
    var deviceSwitchAllowed: (() -> Bool)?

    var selectedDevice: DiscoveredRingDevice? {
        guard let selectedPeripheralIdentifier else { return nil }
        return discoveredDevices.first { $0.id == selectedPeripheralIdentifier }
    }

    var selectedDeviceIdentity: RingDeviceIdentity? {
        if connectedDeviceIdentity?.peripheralIdentifier == selectedPeripheralIdentifier {
            return connectedDeviceIdentity
        }
        if persistedIdentity?.peripheralIdentifier == selectedPeripheralIdentifier {
            return persistedIdentity
        }
        guard let selected = selectedDevice else { return nil }
        return fallbackIdentity(for: selected)
    }

    var hasSelectedDevice: Bool { selectedPeripheralIdentifier != nil }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var imageCharacteristic: CBCharacteristic?
    private var imuDataCharacteristic: CBCharacteristic?
    private var captureStatusCharacteristic: CBCharacteristic?
    private var deviceInfoCharacteristic: CBCharacteristic?

    private let unwrapper = SampleIDUnwrapper()
    private let frameReassembler = CameraFrameReassembler()
    private var shouldAutoReconnect = true
    private var keepaliveTimer: Timer?
    private var desiredMode: FirmwareMode = .idle
    private var disconnectedThisSession = false
    private var shouldRebaseNextStreamStart = false
    private var imageNotificationsReady = false
    private var imuNotificationsReady = false
    private var statusNotificationsReady = false
    private var captureStatusNotificationsReady = false
    private var deviceInfoReady = false
    private var lastAdvertisedNames: [UUID: String] = [:]
    private var peripheralsByID: [UUID: CBPeripheral] = [:]
    private var selectedPeripheralIdentifier: UUID?
    private var persistedIdentity: RingDeviceIdentity?
    private var pendingSwitchIdentifier: UUID?
    private var modeAfterSwitch: FirmwareMode = .idle
    private var switchTimeoutWorkItem: DispatchWorkItem?
    private var cameraRecoveryTimeoutWorkItem: DispatchWorkItem?

    private enum DefaultsKey {
        static let selectedPeripheralID = "FingLab.selectedPeripheralID"
        static let selectedIdentity = "FingLab.selectedDeviceIdentity"
    }

    override init() {
        super.init()
        let defaults = UserDefaults.standard
        if let value = defaults.string(forKey: DefaultsKey.selectedPeripheralID) {
            selectedPeripheralIdentifier = UUID(uuidString: value)
        }
        if let data = defaults.data(forKey: DefaultsKey.selectedIdentity) {
            persistedIdentity = try? JSONDecoder().decode(RingDeviceIdentity.self, from: data)
        }
        if let identity = persistedIdentity {
            discoveredDevices = [DiscoveredRingDevice(
                id: identity.peripheralIdentifier,
                advertisedName: identity.advertisedName,
                rssi: 0,
                lastSeen: .distantPast,
                identity: identity
            )]
        }
        central = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [CBCentralManagerOptionRestoreIdentifierKey: BLEConstants.restoreIdentifier]
        )
    }

    func setAutoReconnect(_ enabled: Bool) { shouldAutoReconnect = enabled }

    func connect() {
        shouldAutoReconnect = true
        if peripheral?.state == .connected {
            connectionState = .connected
            activateDesiredModeIfReady()
            return
        }
        startScanning()
    }

    func startScanning() {
        guard central.state == .poweredOn else { return }

        // CoreBluetooth can scan while one peripheral remains connected. This
        // is necessary for the picker to discover a second ring before the
        // current ring is safely moved to IDLE and disconnected.
        if peripheral?.state == .connected {
            central.scanForPeripherals(
                withServices: [BLEConstants.serviceUUID],
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
            )
            return
        }

        if let selectedPeripheralIdentifier,
           let connected = central.retrieveConnectedPeripherals(
               withServices: [BLEConstants.serviceUUID]
           ).first(where: { $0.identifier == selectedPeripheralIdentifier }) {
            peripheralsByID[connected.identifier] = connected
            connectToKnownPeripheral(connected)
            return
        }

        if connectionState != .connecting && connectionState != .connected {
            connectionState = .scanning
        }
        central.scanForPeripherals(
            withServices: [BLEConstants.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    func stopScanning() {
        central.stopScan()
        if connectionState == .scanning {
            connectionState = peripheral == nil ? .idle : connectionState
        }
    }

    @discardableResult
    func selectDevice(_ device: DiscoveredRingDevice) -> Bool {
        if deviceSwitchAllowed?() == false {
            errorMessage = "Finish or cancel the active IMU session before changing rings."
            return false
        }

        selectedPeripheralIdentifier = device.id
        UserDefaults.standard.set(device.id.uuidString,
                                  forKey: DefaultsKey.selectedPeripheralID)
        errorMessage = nil
        shouldAutoReconnect = true

        if peripheral?.identifier == device.id {
            activateDesiredModeIfReady()
            return true
        }

        guard let current = peripheral, current.state == .connected else {
            connectSelectedDeviceIfAvailable()
            return true
        }

        pendingSwitchIdentifier = device.id
        modeAfterSwitch = desiredMode
        desiredMode = .idle
        stopKeepalive()
        writeCommand(BLEConstants.commandIdle)
        if activeMode == .idle {
            finishPendingSwitch()
        } else {
            let timeout = DispatchWorkItem { [weak self] in self?.finishPendingSwitch() }
            switchTimeoutWorkItem?.cancel()
            switchTimeoutWorkItem = timeout
            DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: timeout)
        }
        return true
    }

    func disconnect() {
        shouldAutoReconnect = false
        pendingSwitchIdentifier = nil
        switchTimeoutWorkItem?.cancel()
        stopKeepalive()
        requestMode(.idle)
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
    }

    func resetForNewSession() {
        unwrapper.reset()
        lastUnwrappedSampleID = nil
        disconnectedThisSession = false
        shouldRebaseNextStreamStart = false
    }

    func startStreaming() { requestMode(.stream) }
    func stopStreaming() { requestMode(.idle) }
    func enterCaptureMode() { requestMode(.capture) }
    func enterIdleMode() { requestMode(.idle) }
    func resendLastImage() { writeCommand(BLEConstants.commandResendLastImage) }

    func recoverCamera() {
        guard connectionState.isConnected, activeMode == .capture else {
            errorMessage = "Connect to the ring in capture mode before recovering the camera."
            return
        }
        guard !isCameraRecoveryInProgress else { return }

        /* Any partial Bayer frame belongs to the failed pipeline and must not
         * be combined with packets received after recovery. */
        frameReassembler.reset()
        isCameraRecoveryInProgress = true
        writeCommand(BLEConstants.commandRecoverCamera)

        // This only restores the button if firmware cannot answer. It does
        // not initiate automatic recovery or reboot the MCU.
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.isCameraRecoveryInProgress else { return }
            self.isCameraRecoveryInProgress = false
            self.errorMessage = "The ring did not report a camera recovery result."
        }
        cameraRecoveryTimeoutWorkItem?.cancel()
        cameraRecoveryTimeoutWorkItem = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 12, execute: timeout)
    }

    func acknowledgeFrame(_ frameID: UInt32) {
        var data = Data([BLEConstants.commandAcknowledgeFrame])
        data.append(contentsOf: [
            UInt8(truncatingIfNeeded: frameID),
            UInt8(truncatingIfNeeded: frameID >> 8),
            UInt8(truncatingIfNeeded: frameID >> 16),
            UInt8(truncatingIfNeeded: frameID >> 24),
        ])
        writeCommand(data)
    }

    func requestMode(_ mode: FirmwareMode) {
        if pendingSwitchIdentifier != nil {
            modeAfterSwitch = mode
            return
        }
        desiredMode = mode
        if mode != .stream { stopKeepalive() }
        activateDesiredModeIfReady()
    }

    private func connectSelectedDeviceIfAvailable() {
        guard let selectedPeripheralIdentifier else {
            startScanning()
            return
        }
        if let found = peripheralsByID[selectedPeripheralIdentifier] {
            connectToKnownPeripheral(found)
        } else {
            startScanning()
        }
    }

    private func connectToKnownPeripheral(_ found: CBPeripheral) {
        guard found.identifier == selectedPeripheralIdentifier else { return }
        stopScanning()
        peripheral = found
        found.delegate = self
        resetDiscoveredCharacteristics()
        connectionState = .connecting
        central.connect(found)
    }

    private func finishPendingSwitch() {
        guard pendingSwitchIdentifier != nil else { return }
        switchTimeoutWorkItem?.cancel()
        switchTimeoutWorkItem = nil
        if let peripheral, peripheral.state != .disconnected {
            central.cancelPeripheralConnection(peripheral)
        } else {
            completePendingSwitchAfterDisconnect()
        }
    }

    private func completePendingSwitchAfterDisconnect() {
        guard let target = pendingSwitchIdentifier else { return }
        pendingSwitchIdentifier = nil
        desiredMode = modeAfterSwitch
        if let found = peripheralsByID[target] {
            connectToKnownPeripheral(found)
        } else {
            startScanning()
        }
    }

    private func activateDesiredModeIfReady() {
        guard commandCharacteristic != nil, deviceInfoReady else { return }
        if desiredMode != .idle && !statusNotificationsReady { return }
        if desiredMode == .capture &&
            (!imageNotificationsReady || !captureStatusNotificationsReady) { return }
        if desiredMode == .stream && !imuNotificationsReady { return }

        if desiredMode == .stream && shouldRebaseNextStreamStart {
            unwrapper.markFirmwareWillRestart()
            shouldRebaseNextStreamStart = false
        }
        writeCommand(desiredMode.rawValue)
        if desiredMode == .stream {
            isStreaming = true
            startKeepalive()
        } else {
            isStreaming = false
        }
    }

    private func writeCommand(_ byte: UInt8) {
        writeCommand(Data([byte]))
    }

    private func writeCommand(_ data: Data) {
        guard let peripheral, let commandCharacteristic else { return }
        peripheral.writeValue(data, for: commandCharacteristic, type: .withResponse)
    }

    private func startKeepalive() {
        stopKeepalive()
        keepaliveTimer = Timer.scheduledTimer(withTimeInterval: BLEConstants.keepaliveInterval,
                                               repeats: true) { [weak self] _ in
            self?.writeCommand(BLEConstants.commandKeepalive)
        }
    }

    private func stopKeepalive() {
        keepaliveTimer?.invalidate()
        keepaliveTimer = nil
    }

    private func resetDiscoveredCharacteristics() {
        commandCharacteristic = nil
        statusCharacteristic = nil
        imageCharacteristic = nil
        imuDataCharacteristic = nil
        captureStatusCharacteristic = nil
        deviceInfoCharacteristic = nil
        imageNotificationsReady = false
        imuNotificationsReady = false
        statusNotificationsReady = false
        captureStatusNotificationsReady = false
        deviceInfoReady = false
    }

    private func updateDiscoveredDevice(_ device: DiscoveredRingDevice) {
        if let index = discoveredDevices.firstIndex(where: { $0.id == device.id }) {
            discoveredDevices[index] = device
        } else {
            discoveredDevices.append(device)
        }
        discoveredDevices.sort {
            if $0.id == selectedPeripheralIdentifier { return true }
            if $1.id == selectedPeripheralIdentifier { return false }
            return $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending
        }
    }

    private func fallbackIdentity(for device: DiscoveredRingDevice) -> RingDeviceIdentity {
        let size = device.ringSizeLabel.flatMap {
            Int($0.replacingOccurrences(of: " mm", with: ""))
        } ?? 0
        return RingDeviceIdentity(
            protocolVersion: 0,
            boardID: device.advertisedName.lowercased(),
            ringSizeMm: size,
            firmwareVersion: "unknown",
            peripheralIdentifier: device.id,
            advertisedName: device.advertisedName
        )
    }

    private func storeDeviceInfo(_ data: Data, for peripheral: CBPeripheral) {
        guard let value = String(data: data, encoding: .utf8) else { return }
        let fields = value.split(separator: "|", omittingEmptySubsequences: false)
        guard fields.count == 4,
              let protocolVersion = Int(fields[0]),
              let ringSize = Int(fields[2]) else {
            errorMessage = "The selected ring returned invalid device information."
            return
        }
        let advertisedName = lastAdvertisedNames[peripheral.identifier]
            ?? peripheral.name ?? String(fields[1]).uppercased()
        let identity = RingDeviceIdentity(
            protocolVersion: protocolVersion,
            boardID: String(fields[1]),
            ringSizeMm: ringSize,
            firmwareVersion: String(fields[3]),
            peripheralIdentifier: peripheral.identifier,
            advertisedName: advertisedName
        )
        connectedDeviceIdentity = identity
        connectedPeripheralName = identity.displayName
        persistedIdentity = identity
        if let encoded = try? JSONEncoder().encode(identity) {
            UserDefaults.standard.set(encoded, forKey: DefaultsKey.selectedIdentity)
        }

        let previous = discoveredDevices.first { $0.id == peripheral.identifier }
        updateDiscoveredDevice(DiscoveredRingDevice(
            id: peripheral.identifier,
            advertisedName: advertisedName,
            rssi: previous?.rssi ?? 0,
            lastSeen: previous?.lastSeen ?? Date(),
            identity: identity
        ))
        deviceInfoReady = true
        activateDesiredModeIfReady()
    }
}

extension RingBLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if shouldAutoReconnect { startScanning() }
        case .unauthorized: connectionState = .unauthorized
        case .poweredOff:
            connectionState = .poweredOff
            activeMode = .idle
            isStreaming = false
            stopKeepalive()
        default: break
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        guard let selectedPeripheralIdentifier,
              let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?
                .first(where: { $0.identifier == selectedPeripheralIdentifier })
        else { return }
        peripheralsByID[restored.identifier] = restored
        if restored.state == .connected {
            peripheral = restored
            restored.delegate = self
            connectionState = .connecting
            restored.discoverServices([BLEConstants.serviceUUID])
        } else {
            connectToKnownPeripheral(restored)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let liveName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let name = liveName ?? peripheral.name ?? ""
        guard name.hasPrefix(BLEConstants.deviceNamePrefix) else { return }
        lastAdvertisedNames[peripheral.identifier] = name
        peripheralsByID[peripheral.identifier] = peripheral
        let existingIdentity = discoveredDevices.first {
            $0.id == peripheral.identifier
        }?.identity
        updateDiscoveredDevice(DiscoveredRingDevice(
            id: peripheral.identifier,
            advertisedName: name,
            rssi: RSSI.intValue,
            lastSeen: Date(),
            identity: existingIdentity
        ))

        if peripheral.identifier == selectedPeripheralIdentifier,
           self.peripheral?.state != .connected,
           connectionState != .connecting {
            connectToKnownPeripheral(peripheral)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard peripheral.identifier == selectedPeripheralIdentifier else {
            central.cancelPeripheralConnection(peripheral)
            return
        }
        connectionState = .connected
        connectedPeripheralName = lastAdvertisedNames[peripheral.identifier] ?? peripheral.name
        peripheral.discoverServices([BLEConstants.serviceUUID])
        if disconnectedThisSession { onConnectionEvent?("reconnect", lastUnwrappedSampleID) }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        errorMessage = error?.localizedDescription
        connectionState = .disconnected
        if shouldAutoReconnect { startScanning() }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        self.peripheral = nil
        resetDiscoveredCharacteristics()
        frameReassembler.reset()
        activeMode = .idle
        isStreaming = false
        isCameraRecoveryInProgress = false
        cameraRecoveryTimeoutWorkItem?.cancel()
        cameraRecoveryTimeoutWorkItem = nil
        connectedDeviceIdentity = nil
        stopKeepalive()
        connectionState = .disconnected
        disconnectedThisSession = true
        shouldRebaseNextStreamStart = desiredMode == .stream || modeAfterSwitch == .stream
        onConnectionEvent?("disconnect", lastUnwrappedSampleID)

        if pendingSwitchIdentifier != nil {
            completePendingSwitchAfterDisconnect()
        } else if shouldAutoReconnect {
            connectionState = .reconnecting
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                self?.startScanning()
            }
        }
    }
}

extension RingBLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == BLEConstants.commandUUID,
              let error else { return }
        isCameraRecoveryInProgress = false
        cameraRecoveryTimeoutWorkItem?.cancel()
        cameraRecoveryTimeoutWorkItem = nil
        errorMessage = "Could not send firmware command: \(error.localizedDescription)"
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil,
              let service = peripheral.services?.first(where: { $0.uuid == BLEConstants.serviceUUID })
        else { return }
        peripheral.discoverCharacteristics([
            BLEConstants.imageDataUUID,
            BLEConstants.statusUUID,
            BLEConstants.imuDataUUID,
            BLEConstants.captureStatusUUID,
            BLEConstants.deviceInfoUUID,
            BLEConstants.commandUUID,
        ], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil, let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            switch characteristic.uuid {
            case BLEConstants.commandUUID:
                commandCharacteristic = characteristic
            case BLEConstants.statusUUID:
                statusCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                peripheral.readValue(for: characteristic)
            case BLEConstants.imageDataUUID:
                imageCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            case BLEConstants.imuDataUUID:
                imuDataCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            case BLEConstants.captureStatusUUID:
                captureStatusCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                peripheral.readValue(for: characteristic)
            case BLEConstants.deviceInfoUUID:
                deviceInfoCharacteristic = characteristic
                peripheral.readValue(for: characteristic)
            default: break
            }
        }
        if deviceInfoCharacteristic == nil {
            errorMessage = "This ring firmware does not expose a stable device identity."
        }
        activateDesiredModeIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil else { return }
        switch characteristic.uuid {
        case BLEConstants.imageDataUUID: imageNotificationsReady = characteristic.isNotifying
        case BLEConstants.imuDataUUID: imuNotificationsReady = characteristic.isNotifying
        case BLEConstants.statusUUID: statusNotificationsReady = characteristic.isNotifying
        case BLEConstants.captureStatusUUID:
            captureStatusNotificationsReady = characteristic.isNotifying
            if characteristic.isNotifying {
                peripheral.readValue(for: characteristic)
            }
        default: return
        }
        activateDesiredModeIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let data = characteristic.value else { return }

        if characteristic.uuid == BLEConstants.deviceInfoUUID {
            storeDeviceInfo(data, for: peripheral)
            return
        }

        if characteristic.uuid == BLEConstants.statusUUID {
            guard data.count == 1, let mode = FirmwareMode(rawValue: data[0]) else { return }
            activeMode = mode
            isStreaming = mode == .stream
            if mode == .idle && pendingSwitchIdentifier != nil {
                finishPendingSwitch()
            }
            return
        }

        if characteristic.uuid == BLEConstants.imageDataUUID {
            guard activeMode == .capture else { return }
            if let frame = frameReassembler.process(data) { onFrameReceived?(frame) }
            return
        }

        if characteristic.uuid == BLEConstants.captureStatusUUID {
            guard let status = CaptureStatus(data: data) else { return }
            latestCaptureStatus = status
            switch status.event {
            case .cameraRecoveryStarted:
                isCameraRecoveryInProgress = true
            case .cameraRecoveryComplete, .cameraRecoveryFailed:
                isCameraRecoveryInProgress = false
                cameraRecoveryTimeoutWorkItem?.cancel()
                cameraRecoveryTimeoutWorkItem = nil
            default:
                break
            }
            onCaptureStatusReceived?(status)
            if status.event == .cameraDiagnostic,
               let previous = captureStatusHistory.first,
               previous.event == .cameraDiagnostic,
               previous.attemptID == status.attemptID,
               previous.detail == status.detail,
               previous.progress == status.progress,
               previous.total == status.total {
                // A diagnostic heartbeat repeats while a camera call is
                // blocked. Replace its timestamp instead of flooding history.
                captureStatusHistory[0] = status
            } else if captureStatusHistory.first != status {
                captureStatusHistory.insert(status, at: 0)
                if captureStatusHistory.count > 12 {
                    captureStatusHistory.removeLast(captureStatusHistory.count - 12)
                }
            }
            return
        }

        guard characteristic.uuid == BLEConstants.imuDataUUID,
              activeMode == .stream else { return }
        let samples = IMUSampleParser.parseSamples(from: data, unwrapper: unwrapper)
        guard !samples.isEmpty else { return }
        lastUnwrappedSampleID = samples.last?.unwrappedSampleID
        onSamplesReceived?(samples)
    }
}
