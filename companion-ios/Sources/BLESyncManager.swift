import CoreBluetooth
import Foundation

final class BLESyncManager: NSObject {
    struct State {
        var bluetoothReady = false
        var bluetoothText = "Starting"
        var syncText = "Waiting for X3"
        var walletText = "Open Ethereum Wallet on X3"
        var walletAddress = "Not connected"
    }

    private static let calendarServiceUUID = CBUUID(string: "7d2ea28a-f7bd-485a-bd9d-92ad6ecfe93e")
    private static let calendarControlUUID = CBUUID(string: "7d2ea28b-f7bd-485a-bd9d-92ad6ecfe93e")
    private static let calendarDataUUID = CBUUID(string: "7d2ea28c-f7bd-485a-bd9d-92ad6ecfe93e")
    private static let calendarStatusUUID = CBUUID(string: "7d2ea28d-f7bd-485a-bd9d-92ad6ecfe93e")
    private static let walletServiceUUID = CBUUID(string: "38178710-0a1b-4f29-9803-7f6a6d75de10")
    private static let walletControlUUID = CBUUID(string: "38178711-0a1b-4f29-9803-7f6a6d75de10")
    private static let walletDataUUID = CBUUID(string: "38178712-0a1b-4f29-9803-7f6a6d75de10")
    private static let walletStatusUUID = CBUUID(string: "38178713-0a1b-4f29-9803-7f6a6d75de10")

    private let stateHandler: (State) -> Void
    private let snapshotProvider: () async throws -> Data
    private let walletResultHandler: (UInt32, Data, Data) -> Void
    private let walletFailureHandler: (String) -> Void
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var calendarControl: CBCharacteristic?
    private var calendarData: CBCharacteristic?
    private var calendarStatus: CBCharacteristic?
    private var walletControl: CBCharacteristic?
    private var walletData: CBCharacteristic?
    private var walletStatus: CBCharacteristic?
    private var state = State()
    private var writeQueue: [(CBCharacteristic, Data)] = []
    private var preparingSnapshot = false
    private var sendingWallet = false
    private var pendingWalletRequest: Data?

    init(state: @escaping (State) -> Void, snapshot: @escaping () async throws -> Data,
         walletResult: @escaping (UInt32, Data, Data) -> Void,
         walletFailure: @escaping (String) -> Void) {
        stateHandler = state
        snapshotProvider = snapshot
        walletResultHandler = walletResult
        walletFailureHandler = walletFailure
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [CBCentralManagerOptionRestoreIdentifierKey: "com.teddyknox.X3Companion.central"]
        )
    }

    func start(forceReconnect: Bool = false) {
        guard central.state == .poweredOn else { return }
        if forceReconnect, let peripheral { central.cancelPeripheralConnection(peripheral) }
        scan()
    }

    func signWallet(request: Data) {
        pendingWalletRequest = request
        update(walletText: "Looking for X3 wallet")
        start(forceReconnect: true)
    }

    private func scan() {
        guard central.state == .poweredOn else { return }
        central.scanForPeripherals(
            withServices: [Self.calendarServiceUUID, Self.walletServiceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        update(bluetoothReady: true, bluetoothText: "On", syncText: "Waiting for X3 wake")
    }

    private func prepareSnapshotIfReady() {
        guard !preparingSnapshot, !sendingWallet,
              let peripheral, let calendarControl, let calendarData else { return }
        preparingSnapshot = true
        update(syncText: "Preparing calendar")
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let snapshot = try await snapshotProvider()
                try enqueue(payload: snapshot, expectedSize: CalendarSnapshotEncoder.wireSize,
                            peripheral: peripheral, control: calendarControl, data: calendarData, wallet: false)
            } catch {
                preparingSnapshot = false
                update(syncText: error.localizedDescription)
            }
        }
    }

    private func prepareWalletIfReady() {
        guard !sendingWallet, !preparingSnapshot, let request = pendingWalletRequest,
              let peripheral, let walletControl, let walletData else { return }
        do {
            try enqueue(payload: request, expectedSize: EvmSignRequestEncoder.wireSize, peripheral: peripheral,
                        control: walletControl, data: walletData, wallet: true)
        } catch {
            pendingWalletRequest = nil
            update(walletText: error.localizedDescription)
            walletFailureHandler(error.localizedDescription)
        }
    }

    private func enqueue(payload: Data, expectedSize: Int, peripheral: CBPeripheral,
                         control: CBCharacteristic, data: CBCharacteristic, wallet: Bool) throws {
        guard payload.count == expectedSize else { throw CompanionError.unavailable }
        writeQueue.removeAll(keepingCapacity: true)
        var begin = Data([1])
        begin.append(UInt8(truncatingIfNeeded: payload.count))
        begin.append(UInt8(truncatingIfNeeded: payload.count >> 8))
        writeQueue.append((control, begin))
        let chunkLength = max(1, min(242, peripheral.maximumWriteValueLength(for: .withResponse) - 2))
        var offset = 0
        while offset < payload.count {
            let end = min(payload.count, offset + chunkLength)
            var packet = Data()
            packet.append(UInt8(truncatingIfNeeded: offset))
            packet.append(UInt8(truncatingIfNeeded: offset >> 8))
            packet.append(payload[offset..<end])
            writeQueue.append((data, packet))
            offset = end
        }
        writeQueue.append((control, Data([UInt8(2)])))
        sendingWallet = wallet
        if wallet { update(walletText: "Sending signing request") } else { update(syncText: "Sending calendar") }
        writeNext()
    }

    private func writeNext() {
        guard let peripheral, !writeQueue.isEmpty else {
            if sendingWallet {
                update(walletText: "Review signing request on X3")
            } else {
                preparingSnapshot = false
                update(syncText: "Waiting for X3 confirmation")
            }
            return
        }
        let (characteristic, data) = writeQueue.removeFirst()
        peripheral.writeValue(data, for: characteristic, type: .withResponse)
    }

    private func update(bluetoothReady: Bool? = nil, bluetoothText: String? = nil,
                        syncText: String? = nil, walletText: String? = nil, walletAddress: String? = nil) {
        if let bluetoothReady { state.bluetoothReady = bluetoothReady }
        if let bluetoothText { state.bluetoothText = bluetoothText }
        if let syncText { state.syncText = syncText }
        if let walletText { state.walletText = walletText }
        if let walletAddress { state.walletAddress = walletAddress }
        stateHandler(state)
    }

    private func handleCalendarStatus(_ data: Data) {
        guard data.count == 12 else { return }
        switch data[1] {
        case 1: update(syncText: "X3 is advertising")
        case 2: update(syncText: "X3 connected")
        case 3: update(syncText: "Confirm pairing on iPhone")
        case 4, 5: update(syncText: "Sending calendar")
        case 6: update(syncText: "Calendar updated")
        case 7: update(syncText: "X3 rejected snapshot (error \(data[2]))")
        default: break
        }
    }

    private func handleWalletStatus(_ data: Data) {
        guard data.count == 129 else { return }
        let requestID = UInt32(data[8]) | UInt32(data[9]) << 8 | UInt32(data[10]) << 16 | UInt32(data[11]) << 24
        let address = "0x" + data[12..<32].map { String(format: "%02x", $0) }.joined()
        update(walletAddress: address)
        switch data[1] {
        case 1: update(walletText: "X3 wallet is advertising")
        case 2: update(walletText: "X3 wallet connected")
        case 3: update(walletText: "Confirm pairing on iPhone")
        case 4: update(walletText: "Sending signing request")
        case 5: update(walletText: "Review signing request on X3")
        case 6:
            let digest = Data(data[32..<64])
            let signature = Data(data[64..<129])
            pendingWalletRequest = nil
            sendingWallet = false
            update(walletText: "Request signed")
            walletResultHandler(requestID, digest, signature)
        case 7:
            pendingWalletRequest = nil
            sendingWallet = false
            update(walletText: "Signing rejected")
            walletFailureHandler("Signing request rejected on X3")
        case 8:
            pendingWalletRequest = nil
            sendingWallet = false
            update(walletText: "X3 wallet error \(data[2])")
            walletFailureHandler("X3 wallet error \(data[2])")
        default: break
        }
    }

    private func clearConnection() {
        peripheral = nil
        calendarControl = nil
        calendarData = nil
        calendarStatus = nil
        walletControl = nil
        walletData = nil
        walletStatus = nil
        writeQueue.removeAll(keepingCapacity: true)
        preparingSnapshot = false
        sendingWallet = false
    }
}

extension BLESyncManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn: scan()
        case .poweredOff: update(bluetoothReady: false, bluetoothText: "Off", syncText: "Turn on Bluetooth")
        case .unauthorized: update(bluetoothReady: false, bluetoothText: "Not allowed", syncText: "Allow Bluetooth access")
        case .unsupported: update(bluetoothReady: false, bluetoothText: "Unsupported", syncText: "BLE is unavailable")
        default: update(bluetoothReady: false, bluetoothText: "Starting")
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        guard let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first else { return }
        peripheral = restored
        restored.delegate = self
        if restored.state == .connected {
            restored.discoverServices([Self.calendarServiceUUID, Self.walletServiceUUID])
        } else {
            central.connect(restored)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        self.peripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        update(syncText: "Connecting to X3")
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.calendarServiceUUID, Self.walletServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        update(syncText: error?.localizedDescription ?? "Could not connect")
        scan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        timestamp: CFAbsoluteTime, isReconnecting: Bool, error: Error?) {
        clearConnection()
        scan()
    }
}

extension BLESyncManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil else {
            update(syncText: error?.localizedDescription ?? "X3 service not found")
            return
        }
        for service in peripheral.services ?? [] {
            if service.uuid == Self.calendarServiceUUID {
                peripheral.discoverCharacteristics(
                    [Self.calendarControlUUID, Self.calendarDataUUID, Self.calendarStatusUUID], for: service)
            } else if service.uuid == Self.walletServiceUUID {
                peripheral.discoverCharacteristics(
                    [Self.walletControlUUID, Self.walletDataUUID, Self.walletStatusUUID], for: service)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil else {
            update(syncText: error?.localizedDescription ?? "Could not discover X3 controls")
            return
        }
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case Self.calendarControlUUID: calendarControl = characteristic
            case Self.calendarDataUUID: calendarData = characteristic
            case Self.calendarStatusUUID:
                calendarStatus = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                peripheral.readValue(for: characteristic)
            case Self.walletControlUUID: walletControl = characteristic
            case Self.walletDataUUID: walletData = characteristic
            case Self.walletStatusUUID:
                walletStatus = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                peripheral.readValue(for: characteristic)
            default: break
            }
        }
        if service.uuid == Self.walletServiceUUID { prepareWalletIfReady() } else { prepareSnapshotIfReady() }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            let walletWasSending = sendingWallet
            preparingSnapshot = false
            sendingWallet = false
            if walletWasSending { pendingWalletRequest = nil }
            writeQueue.removeAll(keepingCapacity: true)
            update(syncText: error.localizedDescription, walletText: error.localizedDescription)
            if walletWasSending { walletFailureHandler(error.localizedDescription) }
            return
        }
        writeNext()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let value = characteristic.value else { return }
        if characteristic.uuid == Self.calendarStatusUUID {
            handleCalendarStatus(value)
        } else if characteristic.uuid == Self.walletStatusUUID {
            handleWalletStatus(value)
        }
    }
}
