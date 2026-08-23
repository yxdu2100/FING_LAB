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

    var onSamplesReceived: (([IMUSample]) -> Void)?
    var onConnectionEvent: ((String, UInt64?) -> Void)?
    var onFrameReceived: ((ReassembledFrame) -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var imageCharacteristic: CBCharacteristic?
    private var imuDataCharacteristic: CBCharacteristic?

    private let unwrapper = SampleIDUnwrapper()
    private let frameReassembler = CameraFrameReassembler()
    private var shouldAutoReconnect = true
    private var keepaliveTimer: Timer?
    private var desiredMode: FirmwareMode = .idle
    private var lastPeripheralIdentifier: UUID?
    private var disconnectedThisSession = false
    private var shouldRebaseNextStreamStart = false
    private var imageNotificationsReady = false
    private var imuNotificationsReady = false
    private var statusNotificationsReady = false
    private var lastAdvertisedName: String?

    override init() {
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [CBCentralManagerOptionRestoreIdentifierKey: BLEConstants.restoreIdentifier]
        )
    }

    func setAutoReconnect(_ enabled: Bool) { shouldAutoReconnect = enabled }

    func connect() {
        if peripheral?.state == .connected {
            connectionState = .connected
            activateDesiredModeIfReady()
            return
        }
        startScanning()
    }

    func startScanning() {
        guard central.state == .poweredOn else { return }
        if let connected = central.retrieveConnectedPeripherals(withServices: [BLEConstants.serviceUUID]).first {
            connectToKnownPeripheral(connected)
            return
        }
        connectionState = .scanning
        central.scanForPeripherals(withServices: [BLEConstants.serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func stopScanning() {
        central.stopScan()
        if connectionState == .scanning {
            connectionState = peripheral == nil ? .idle : connectionState
        }
    }

    func disconnect() {
        shouldAutoReconnect = false
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

    func requestMode(_ mode: FirmwareMode) {
        desiredMode = mode
        if mode != .stream { stopKeepalive() }
        activateDesiredModeIfReady()
    }

    private func connectToKnownPeripheral(_ found: CBPeripheral) {
        stopScanning()
        peripheral = found
        found.delegate = self
        lastPeripheralIdentifier = found.identifier
        connectionState = .connecting
        central.connect(found)
    }

    private func activateDesiredModeIfReady() {
        guard commandCharacteristic != nil else { return }
        // Do not switch a data-producing mode until its acknowledgement
        // channel is subscribed. Otherwise the first status notification can
        // be lost and the app would correctly reject all incoming payloads.
        if desiredMode != .idle && !statusNotificationsReady { return }
        if desiredMode == .capture && !imageNotificationsReady { return }
        if desiredMode == .stream && !imuNotificationsReady { return }

        if desiredMode == .stream && shouldRebaseNextStreamStart {
            // The firmware restarts its 16-bit sample counter whenever STREAM is
            // entered. Rebase once after reconnect so the exported IDs remain
            // monotonic instead of looking like a 65,536-sample jump.
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
        guard let peripheral, let commandCharacteristic else { return }
        peripheral.writeValue(Data([byte]), for: commandCharacteristic, type: .withoutResponse)
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
        guard let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first else { return }
        if restored.state == .connected {
            peripheral = restored
            restored.delegate = self
            lastPeripheralIdentifier = restored.identifier
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
        guard name == BLEConstants.deviceName || name.hasPrefix(BLEConstants.deviceName) else { return }
        lastAdvertisedName = liveName
        connectToKnownPeripheral(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectionState = .connected
        connectedPeripheralName = lastAdvertisedName ?? peripheral.name
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
        commandCharacteristic = nil
        statusCharacteristic = nil
        imageCharacteristic = nil
        imuDataCharacteristic = nil
        imageNotificationsReady = false
        imuNotificationsReady = false
        statusNotificationsReady = false
        frameReassembler.reset()
        activeMode = .idle
        isStreaming = false
        stopKeepalive()
        connectionState = .disconnected
        disconnectedThisSession = true
        shouldRebaseNextStreamStart = desiredMode == .stream
        onConnectionEvent?("disconnect", lastUnwrappedSampleID)

        if shouldAutoReconnect {
            connectionState = .reconnecting
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in self?.startScanning() }
        }
    }
}

extension RingBLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil,
              let service = peripheral.services?.first(where: { $0.uuid == BLEConstants.serviceUUID })
        else { return }
        peripheral.discoverCharacteristics([
            BLEConstants.imageDataUUID,
            BLEConstants.statusUUID,
            BLEConstants.imuDataUUID,
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
            default: break
            }
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
        default: return
        }
        activateDesiredModeIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let data = characteristic.value else { return }

        if characteristic.uuid == BLEConstants.statusUUID {
            guard data.count == 1, let mode = FirmwareMode(rawValue: data[0]) else { return }
            activeMode = mode
            isStreaming = mode == .stream
            return
        }

        if characteristic.uuid == BLEConstants.imageDataUUID {
            guard activeMode == .capture else { return }
            if let frame = frameReassembler.process(data) { onFrameReceived?(frame) }
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
