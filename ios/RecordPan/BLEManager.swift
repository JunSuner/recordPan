import Combine
import CoreBluetooth
import Foundation

/// Finds the recorder, keeps the connection alive and exposes its status, file list
/// and live audio. All delegate callbacks run on the main queue.
final class BLEManager: NSObject, ObservableObject {
    enum Link: Equatable {
        case bluetoothOff
        case scanning
        case connecting
        case connected(String)
    }

    @Published private(set) var link: Link = .bluetoothOff
    @Published private(set) var status: DeviceStatus?
    @Published private(set) var files: [RecordingFile] = []

    /// Called with every audio notification while recording.
    var onAudio: ((AudioChunk) -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristics: [CBUUID: CBCharacteristic] = [:]
    private var incomingFiles: [RecordingFile] = []

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var isConnected: Bool {
        if case .connected = link { return true }
        return false
    }

    func send(_ command: RP.Command) {
        guard let peripheral, let control = characteristics[RP.control] else { return }
        peripheral.writeValue(Data([command.rawValue]), for: control, type: .withResponse)
    }

    func refreshFiles() {
        incomingFiles = []
        send(.listFiles)
    }

    private func scan() {
        guard central.state == .poweredOn else { return }
        link = .scanning
        central.scanForPeripherals(withServices: [RP.service])
    }

    private func reset() {
        peripheral = nil
        characteristics = [:]
        status = nil
        scan()
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            scan()
        } else {
            link = .bluetoothOff
            peripheral = nil
            status = nil
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        link = .connecting
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([RP.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        reset()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        reset()
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        for service in peripheral.services ?? [] where service.uuid == RP.service {
            peripheral.discoverCharacteristics([RP.control, RP.status, RP.files, RP.audio], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for c in service.characteristics ?? [] {
            characteristics[c.uuid] = c
            if c.uuid != RP.control {
                peripheral.setNotifyValue(true, for: c)
            }
        }
        if let status = characteristics[RP.status] {
            peripheral.readValue(for: status)
        }
        link = .connected(peripheral.name ?? "recordPan")
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        // The list is pushed as notifications, so ask for it once we're subscribed.
        if characteristic.uuid == RP.files, characteristic.isNotifying {
            refreshFiles()
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard let data = characteristic.value else { return }
        switch characteristic.uuid {
        case RP.status:
            if let s = DeviceStatus(data) {
                if status?.state == .recording, s.state == .idle {
                    refreshFiles()  // a new recording was just saved
                }
                status = s
            }
        case RP.files:
            guard data.count >= 4 else { return }
            let size = data.u32(at: 0)
            if data.count == 4, size == 0xFFFF_FFFF {
                files = incomingFiles.sorted { $0.name > $1.name }
                incomingFiles = []
            } else if let name = String(data: data.dropFirst(4), encoding: .utf8) {
                incomingFiles.append(RecordingFile(name: name, size: size))
            }
        case RP.audio:
            if let chunk = AudioChunk(data) {
                onAudio?(chunk)
            }
        default:
            break
        }
    }
}
