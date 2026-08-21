import CoreBluetooth
import Foundation

final class BLECameraTransport: NSObject {
    enum State: Equatable {
        case bluetoothOff
        case scanning
        case connecting(String)
        case discovering
        case ready(String)
        case disconnected

        var description: String {
            switch self {
            case .bluetoothOff: return "Bluetooth unavailable"
            case .scanning: return "Searching for FING_LAB"
            case .connecting(let name): return "Connecting to \(name)"
            case .discovering: return "Discovering image service"
            case .ready(let name): return "Connected to \(name)"
            case .disconnected: return "Disconnected"
            }
        }

        var ready: Bool {
            if case .ready = self { return true }
            return false
        }
    }

    var onStateChange: ((State) -> Void)?
    var onFrame: ((ReassembledFrame) -> Void)?
    var onError: ((String) -> Void)?

    private lazy var central = CBCentralManager(delegate: self, queue: .main)
    private let reassembler = CameraFrameReassembler()
    private var peripheral: CBPeripheral?
    private var imageCharacteristic: CBCharacteristic?

    override init() {
        super.init()
        _ = central
    }

    func reconnect() {
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
        } else {
            startScanning()
        }
    }

    private func startScanning() {
        guard central.state == .poweredOn else {
            onStateChange?(.bluetoothOff)
            return
        }
        central.stopScan()
        onStateChange?(.scanning)
        central.scanForPeripherals(withServices: [CameraUUIDs.service],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }
}

extension BLECameraTransport: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            startScanning()
        } else {
            onStateChange?(.bluetoothOff)
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        let name = peripheral.name ?? "FING_LAB"
        onStateChange?(.connecting(name))
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        reassembler.reset()
        onStateChange?(.discovering)
        peripheral.discoverServices([CameraUUIDs.service])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        onError?(error?.localizedDescription ?? "Could not connect to FING_LAB")
        self.peripheral = nil
        onStateChange?(.disconnected)
        startScanning()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        imageCharacteristic = nil
        self.peripheral = nil
        reassembler.reset()
        onStateChange?(.disconnected)
        startScanning()
    }
}

extension BLECameraTransport: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            onError?(error.localizedDescription)
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == CameraUUIDs.service }) else {
            onError?("FING_LAB image service was not found")
            return
        }
        peripheral.discoverCharacteristics([CameraUUIDs.image], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error {
            onError?(error.localizedDescription)
            return
        }
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == CameraUUIDs.image }) else {
            onError?("Image characteristic was not found")
            return
        }
        imageCharacteristic = characteristic
        peripheral.setNotifyValue(true, for: characteristic)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            onError?(error.localizedDescription)
            return
        }
        if characteristic.uuid == CameraUUIDs.image && characteristic.isNotifying {
            onStateChange?(.ready(peripheral.name ?? "FING_LAB"))
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            onError?(error.localizedDescription)
            return
        }
        guard characteristic.uuid == CameraUUIDs.image,
              let packet = characteristic.value else { return }
        if let frame = reassembler.process(packet) {
            onFrame?(frame)
        }
    }
}
