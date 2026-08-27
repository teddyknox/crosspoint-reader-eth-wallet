import Combine
import CoreBluetooth
import EventKit
import Foundation

@MainActor
final class CompanionModel: ObservableObject {
    @Published private(set) var calendarAccessText = "Checking"
    @Published private(set) var hasCalendarAccess = false
    @Published private(set) var eventCount = 0
    @Published private(set) var bluetoothText = "Starting"
    @Published private(set) var bluetoothReady = false
    @Published private(set) var syncText = "Waiting for X3"
    @Published private(set) var walletText = "Open Ethereum Wallet on X3"
    @Published private(set) var walletAddress = "Not connected"
    @Published private(set) var signedTransaction: String?
    @Published private(set) var walletOperationError: String?
    @Published private(set) var isPreparingWalletTransaction = false
    @Published private(set) var isWalletRequestPending = false
    let rpcSettings = RPCSettings()

    private let calendar = CalendarSnapshotProvider()
    private var pendingRequest: PreparedWalletRequest?
    lazy var walletConnect = WalletConnectManager(
        customRPCProvider: { [weak self] in
            (self?.rpcSettings.customURL, self?.rpcSettings.customChainID)
        }
    ) { [weak self] prepared in
        self?.beginWalletConnectSigning(prepared)
    }
    private lazy var bluetooth = BLESyncManager { [weak self] state in
        Task { @MainActor in self?.apply(state) }
    } snapshot: { [weak self] in
        guard let self else { throw CompanionError.unavailable }
        let result = try await self.calendar.snapshot()
        await MainActor.run { self.eventCount = result.eventCount }
        return result.data
    } walletResult: { [weak self] requestID, _, signature in
        Task { @MainActor in self?.completeWalletRequest(requestID: requestID, signature: signature) }
    } walletFailure: { [weak self] message in
        Task { @MainActor in self?.failWalletRequest(message) }
    }

    func start() async {
        refreshCalendarStatus()
        walletConnect.start()
        bluetooth.start()
        if hasCalendarAccess {
            await refreshEventCount()
        }
    }

    func requestCalendarAccess() async {
        do {
            _ = try await calendar.requestAccess()
        } catch {
            syncText = error.localizedDescription
        }
        refreshCalendarStatus()
        if hasCalendarAccess {
            await refreshEventCount()
            bluetooth.start()
        }
    }

    func syncNow() {
        syncText = "Looking for X3"
        bluetooth.start(forceReconnect: true)
    }

    func signEvm(chainID: String, nonce: String, recipient: String, amount: String,
                 gasLimit: String, priorityFee: String, maxFee: String, data: String,
                 automaticNetworkValues: Bool) async -> AutomaticEvmTransaction? {
        guard pendingRequest == nil, !isPreparingWalletTransaction else {
            walletText = "Another X3 signing request is pending"
            return nil
        }
        isPreparingWalletTransaction = true
        walletOperationError = nil
        defer { isPreparingWalletTransaction = false }
        do {
            let automatic: AutomaticEvmTransaction?
            let prepared: PreparedEvmTransaction
            if automaticNetworkValues {
                guard walletAddress.isUsableEvmAddress else {
                    throw EvmTransactionServiceError.walletUnavailable
                }
                let projectID = walletConnect.projectID.trimmingCharacters(in: .whitespacesAndNewlines)
                guard rpcSettings.customURL != nil || !projectID.isEmpty else {
                    throw EvmTransactionServiceError.missingProjectID
                }
                let service = EvmTransactionService(
                    rpc: ReownRPCClient(
                        projectID: projectID,
                        customURL: rpcSettings.customURL,
                        customChainID: rpcSettings.customChainID
                    )
                )
                let result = try await service.prepare(
                    chainID: chainID,
                    sender: walletAddress,
                    recipient: recipient,
                    amount: amount,
                    dataHex: data
                )
                automatic = result
                prepared = result.prepared
            } else {
                automatic = nil
                prepared = try EvmTransactionEncoder.prepare(
                    chainID: chainID, nonce: nonce, recipient: recipient, amount: amount,
                    gasLimit: gasLimit, maxPriorityFeeGwei: priorityFee, maxFeeGwei: maxFee, dataHex: data
                )
            }
            pendingRequest = .transaction(prepared)
            isWalletRequestPending = true
            signedTransaction = nil
            walletText = "Open Ethereum Wallet on X3"
            bluetooth.signWallet(request: prepared.request)
            return automatic
        } catch {
            walletText = error.localizedDescription
            walletOperationError = error.localizedDescription
            return nil
        }
    }

    private func refreshCalendarStatus() {
        hasCalendarAccess = calendar.hasFullAccess
        calendarAccessText = hasCalendarAccess ? "Allowed" : "Not allowed"
    }

    private func refreshEventCount() async {
        do {
            eventCount = try await calendar.snapshot().eventCount
        } catch {
            syncText = error.localizedDescription
        }
    }

    private func apply(_ state: BLESyncManager.State) {
        bluetoothReady = state.bluetoothReady
        bluetoothText = state.bluetoothText
        syncText = state.syncText
        walletText = state.walletText
        walletAddress = state.walletAddress
        walletConnect.updateWalletAddress(state.walletAddress)
    }

    private func completeWalletRequest(requestID: UInt32, signature: Data) {
        guard let pendingRequest, pendingRequest.requestID == requestID else {
            walletText = "Received a signature for an unknown request"
            return
        }
        do {
            let result = try pendingRequest.complete(signature: signature)
            self.pendingRequest = nil
            isWalletRequestPending = false
            switch result {
            case .transaction(let raw):
                signedTransaction = "0x" + raw.map { String(format: "%02x", $0) }.joined()
                walletText = "Signed raw transaction ready"
            case .signature:
                signedTransaction = nil
                walletText = "Signature returned to dapp"
            }
            walletConnect.completeSigning(result)
        } catch {
            self.pendingRequest = nil
            isWalletRequestPending = false
            walletText = error.localizedDescription
            walletOperationError = error.localizedDescription
            walletConnect.failSigning(error.localizedDescription)
        }
    }

    private func beginWalletConnectSigning(_ prepared: PreparedWalletRequest) {
        guard pendingRequest == nil else {
            walletConnect.failSigning("Another X3 signing request is pending")
            return
        }
        pendingRequest = prepared
        isWalletRequestPending = true
        signedTransaction = nil
        walletOperationError = nil
        walletText = "Open Ethereum Wallet on X3"
        bluetooth.signWallet(request: prepared.request)
    }

    private func failWalletRequest(_ message: String) {
        pendingRequest = nil
        isWalletRequestPending = false
        walletOperationError = message
        walletConnect.failSigning(message)
    }
}

enum CompanionError: LocalizedError {
    case calendarAccessRequired
    case unavailable

    var errorDescription: String? {
        switch self {
        case .calendarAccessRequired: "Calendar access is required"
        case .unavailable: "Calendar snapshot is unavailable"
        }
    }
}
