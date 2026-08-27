import Combine
import Foundation
import ReownWalletKit
import Security
import WalletConnectNetworking
import WalletConnectSign

enum WalletCallBatchError: LocalizedError {
    case invalidParameters
    case unauthorized
    case unsupportedCapability
    case unsupportedAtomicity
    case duplicateID
    case unknownID
    case batchTooLarge

    var code: Int {
        switch self {
        case .invalidParameters: -32602
        case .unauthorized: 4100
        case .unsupportedCapability: 5700
        case .unsupportedAtomicity: 5760
        case .duplicateID: 5720
        case .unknownID: 5730
        case .batchTooLarge: 5740
        }
    }

    var errorDescription: String? {
        switch self {
        case .invalidParameters: "The dapp supplied malformed wallet call parameters"
        case .unauthorized: "The requested account is not authorized in this wallet session"
        case .unsupportedCapability: "The batch requires a capability this X3 wallet does not support"
        case .unsupportedAtomicity: "This EOA wallet cannot execute multiple calls atomically"
        case .duplicateID: "The dapp reused a wallet call batch ID"
        case .unknownID: "The wallet call batch ID is unknown"
        case .batchTooLarge: "The X3 supports between 1 and 8 calls per batch"
        }
    }
}

struct WalletCallBatchRequest {
    let version: String
    let id: String?
    let chainIDHex: String
    let chainIDDecimal: String
    let calls: [EvmWalletCall]

    static func parse(_ value: Any, expectedChainID: String, expectedAddress: String) throws
        -> WalletCallBatchRequest {
        guard let params = value as? [Any], params.count == 1,
              let root = params[0] as? [String: Any],
              let version = root["version"] as? String, version == "2.0.0",
              let chainIDHex = root["chainId"] as? String,
              let chainID = canonicalChainID(chainIDHex), String(chainID) == expectedChainID,
              let atomicRequired = root["atomicRequired"] as? Bool,
              let rawCalls = root["calls"] as? [Any] else {
            throw WalletCallBatchError.invalidParameters
        }
        if root["from"] != nil {
            guard let from = root["from"] as? String else {
                throw WalletCallBatchError.invalidParameters
            }
            guard from.caseInsensitiveCompare(expectedAddress) == .orderedSame else {
                throw WalletCallBatchError.unauthorized
            }
        }
        if atomicRequired { throw WalletCallBatchError.unsupportedAtomicity }
        guard !rawCalls.isEmpty, rawCalls.count <= EvmTransactionService.maximumBatchCalls else {
            throw WalletCallBatchError.batchTooLarge
        }
        try validateCapabilities(root["capabilities"])

        var calls: [EvmWalletCall] = []
        calls.reserveCapacity(rawCalls.count)
        for rawCall in rawCalls {
            guard let call = rawCall as? [String: Any], let recipient = call["to"] as? String else {
                throw WalletCallBatchError.invalidParameters
            }
            try validateCapabilities(call["capabilities"])
            if call["data"] != nil && !(call["data"] is String) ||
                call["value"] != nil && !(call["value"] is String) {
                throw WalletCallBatchError.invalidParameters
            }
            let data = call["data"] as? String ?? "0x"
            let value = call["value"] as? String ?? "0x0"
            calls.append(EvmWalletCall(recipient: recipient, value: value, data: data))
        }

        let id = root["id"] as? String
        if root["id"] != nil && id == nil { throw WalletCallBatchError.invalidParameters }
        if let id {
            let digits = id.dropFirst(2)
            guard id.hasPrefix("0x"), !digits.isEmpty, id.count <= 8_194,
                  digits.count.isMultiple(of: 2), digits.allSatisfy(\.isHexDigit) else {
                throw WalletCallBatchError.invalidParameters
            }
        }
        return WalletCallBatchRequest(
            version: version, id: id, chainIDHex: chainIDHex.lowercased(),
            chainIDDecimal: String(chainID), calls: calls
        )
    }

    private static func canonicalChainID(_ value: String) -> UInt64? {
        guard value.hasPrefix("0x") else { return nil }
        let digits = value.dropFirst(2)
        guard !digits.isEmpty, digits.allSatisfy(\.isHexDigit),
              digits.count == 1 || digits.first != "0" else { return nil }
        return UInt64(digits, radix: 16)
    }

    private static func validateCapabilities(_ value: Any?) throws {
        guard let value else { return }
        guard let capabilities = value as? [String: Any] else {
            throw WalletCallBatchError.invalidParameters
        }
        for rawCapability in capabilities.values {
            guard let capability = rawCapability as? [String: Any] else {
                throw WalletCallBatchError.invalidParameters
            }
            if capability["optional"] as? Bool != true {
                throw WalletCallBatchError.unsupportedCapability
            }
        }
    }
}

@MainActor
final class WalletConnectManager: ObservableObject {
    typealias SigningHandler = (PreparedWalletRequest) -> Void

    @Published var projectID = UserDefaults.standard.string(forKey: "reownProjectID")
        ?? Bundle.main.object(forInfoDictionaryKey: "X3ReownProjectID") as? String
        ?? ""
    @Published var pairingURI = ""
    @Published private(set) var status = "Add a Reown project ID"
    @Published private(set) var proposalName: String?
    @Published private(set) var proposalURL = ""
    @Published private(set) var proposalMethods: [String] = []
    @Published private(set) var requestSummary: String?
    @Published private(set) var sessionCount = 0

    private static let supportedMethods: Set<String> = [
        "eth_signTransaction", "eth_sendTransaction", "eth_signTypedData_v4", "personal_sign", "eth_sign",
        "wallet_sendCalls", "wallet_getCallsStatus", "wallet_showCallsStatus", "wallet_getCapabilities",
    ]
    private static let supportedEvents: Set<String> = ["accountsChanged", "chainChanged"]

    private var configured = false
    private var walletAddress: String?
    private var pendingProposal: Session.Proposal?
    private var pendingRequest: Request?
    private var pendingMethod: String?
    private var pendingBatch: PendingCallBatch?
    private var batchRecords: [String: CallBatchRecord] = [:]
    private var cancellables = Set<AnyCancellable>()
    private let customRPCProvider: () -> (URL?, String?)
    private let signingHandler: SigningHandler

    private struct PendingCallBatch {
        let id: String
        let version: String
        let chainIDHex: String
        let chainIDDecimal: String
        let transactions: [PreparedEvmTransaction]
        let rpc: ReownRPCClient
        var signedTransactions: [Data] = []
    }

    private struct CallBatchRecord {
        let version: String
        let chainIDHex: String
        let chainIDDecimal: String
        let transactionHashes: [String]
        let submissionFailed: Bool
        let createdAt: Date
        let rpc: ReownRPCClient
    }

    init(customRPCProvider: @escaping () -> (URL?, String?), signingHandler: @escaping SigningHandler) {
        self.customRPCProvider = customRPCProvider
        self.signingHandler = signingHandler
    }

    func start() {
        guard !projectID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        configure()
    }

    func updateWalletAddress(_ address: String) {
        let normalized = address.lowercased()
        guard normalized.hasPrefix("0x"), normalized.count == 42,
              normalized.dropFirst(2).contains(where: { $0 != "0" }) else { return }
        walletAddress = normalized
        if configured { status = "Ready to pair" }
    }

    func configure() {
        let trimmed = projectID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !configured, !trimmed.isEmpty else { return }
        projectID = trimmed
        UserDefaults.standard.set(trimmed, forKey: "reownProjectID")
        let storageGroup = Bundle.main.object(forInfoDictionaryKey: "X3WalletConnectAccessGroup") as? String
            ?? "com.teddyknox.X3Companion"
        Networking.configure(
            groupIdentifier: storageGroup,
            projectId: trimmed,
            socketFactory: X3SocketFactory()
        )
        do {
            let redirect = try AppMetadata.Redirect(native: "x3companion://", universal: nil)
            let metadata = AppMetadata(
                name: "X3 Companion",
                description: "Experimental Xteink X3 hardware signer",
                url: "https://github.com/teddyknox",
                icons: [],
                redirect: redirect
            )
            WalletKit.configure(metadata: metadata, crypto: X3CryptoProvider())
            configured = true
            bind()
            status = walletAddress == nil ? "Open Ethereum Wallet on X3" : "Ready to pair"
        } catch {
            status = error.localizedDescription
        }
    }

    func pair() {
        configure()
        guard configured else { return }
        let text = pairingURI.trimmingCharacters(in: .whitespacesAndNewlines)
        Task {
            do {
                let uri = try WalletConnectURI(uriString: text)
                try await WalletKit.instance.pair(uri: uri)
                status = "Waiting for session proposal"
            } catch {
                status = error.localizedDescription
            }
        }
    }

    func handle(url: URL) {
        _ = pair(scannedPayload: url.absoluteString)
    }

    @discardableResult
    func pair(scannedPayload: String) -> Bool {
        guard let uri = Self.pairingURI(from: scannedPayload) else {
            status = "That QR code is not a WalletConnect pairing request"
            return false
        }
        pairingURI = uri
        pair()
        return true
    }

    nonisolated static func pairingURI(from payload: String) -> String? {
        let trimmed = payload.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.lowercased().hasPrefix("wc:") { return trimmed }
        guard let components = URLComponents(string: trimmed),
              let uri = components.queryItems?.first(where: { $0.name == "uri" })?.value,
              uri.lowercased().hasPrefix("wc:") else { return nil }
        return uri
    }

    func approveProposal() {
        guard let proposal = pendingProposal, let walletAddress else {
            status = "Open Ethereum Wallet on X3 first"
            return
        }
        Task {
            do {
                let required = proposal.requiredNamespaces
                guard required.keys.allSatisfy({ $0 == "eip155" || $0.hasPrefix("eip155:") }),
                      required.values.allSatisfy({ $0.methods.isSubset(of: Self.supportedMethods) &&
                          $0.events.isSubset(of: Self.supportedEvents) }) else {
                    throw WalletConnectBridgeError.unsupportedProposal
                }
                let optionalNamespaces = proposal.optionalNamespaces.map { Array($0.values) } ?? []
                let allNamespaces = Array(required.values) + optionalNamespaces
                let chains = Array(Set(allNamespaces.flatMap { $0.chains ?? [] }.filter { $0.namespace == "eip155" }))
                guard !chains.isEmpty else { throw WalletConnectBridgeError.unsupportedProposal }
                let accounts = chains.compactMap { Account(blockchain: $0, address: walletAddress) }
                let methods = Set(allNamespaces.flatMap(\.methods)).intersection(Self.supportedMethods)
                let events = Set(allNamespaces.flatMap(\.events)).intersection(Self.supportedEvents)
                let namespaces = try AutoNamespaces.build(
                    sessionProposal: proposal,
                    chains: chains,
                    methods: Array(methods),
                    events: Array(events),
                    accounts: accounts
                )
                _ = try await WalletKit.instance.approve(proposalId: proposal.id, namespaces: namespaces)
                pendingProposal = nil
                proposalName = nil
                sessionCount = WalletKit.instance.getSessions().count
                status = "WalletConnect session active"
            } catch {
                status = error.localizedDescription
            }
        }
    }

    func rejectProposal() {
        guard let proposal = pendingProposal else { return }
        Task {
            try? await WalletKit.instance.rejectSession(proposalId: proposal.id, reason: .userRejected)
            pendingProposal = nil
            proposalName = nil
            status = "Session rejected"
        }
    }

    func completeSigning(_ signingResult: WalletSigningResult) {
        guard let request = pendingRequest, let method = pendingMethod else { return }
        if method == "wallet_sendCalls" {
            completeBatchSigning(signingResult, request: request)
            return
        }
        pendingRequest = nil
        pendingMethod = nil
        requestSummary = nil
        Task {
            do {
                let result: String
                switch signingResult {
                case .transaction(let rawTransaction):
                    let rawHex = "0x" + rawTransaction.map { String(format: "%02x", $0) }.joined()
                    if method == "eth_sendTransaction" {
                        let customRPC = customRPCProvider()
                        result = try await ReownRPCClient(
                            projectID: projectID,
                            customURL: customRPC.0,
                            customChainID: customRPC.1
                        ).call(
                            chainID: request.chainId.reference,
                            method: "eth_sendRawTransaction",
                            params: [rawHex]
                        )
                    } else {
                        result = rawHex
                    }
                case .signature(let signature):
                    result = "0x" + signature.map { String(format: "%02x", $0) }.joined()
                }
                try await WalletKit.instance.respond(
                    topic: request.topic,
                    requestId: request.id,
                    response: .response(AnyCodable(result))
                )
                status = method == "eth_sendTransaction" ? "Transaction submitted: \(result)" : "Signature returned"
            } catch {
                status = error.localizedDescription
                await respondError(request, code: -32603, message: error.localizedDescription)
            }
        }
    }

    func failSigning(_ message: String) {
        guard let request = pendingRequest else { return }
        pendingRequest = nil
        pendingMethod = nil
        pendingBatch = nil
        requestSummary = nil
        status = message
        Task { await respondError(request, code: 4001, message: message) }
    }

    private func bind() {
        WalletKit.instance.sessionProposalPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] proposal, _ in
                guard let self else { return }
                pendingProposal = proposal
                proposalName = proposal.proposer.name
                proposalURL = proposal.proposer.url
                proposalMethods = Array(proposal.requiredNamespaces.values.flatMap(\.methods)).sorted()
                status = "Approve or reject the session"
            }
            .store(in: &cancellables)

        WalletKit.instance.sessionRequestPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] request, _ in self?.receive(request) }
            .store(in: &cancellables)

        WalletKit.instance.sessionsPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] sessions in self?.sessionCount = sessions.count }
            .store(in: &cancellables)

        WalletKit.instance.socketConnectionStatusPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connection in
                guard let self else { return }
                if connection == .disconnected, proposalName == nil, pendingRequest == nil {
                    status = "WalletConnect relay disconnected"
                }
            }
            .store(in: &cancellables)
    }

    private func receive(_ request: Request) {
        let peerURL = WalletKit.instance.getSessions().first(where: { $0.topic == request.topic })?.peer.url ?? "missing"
        print("[X3 WC] method=\(request.method) chain=\(request.chainId) " +
              "signerConnected=\(walletAddress != nil) peerMetadata=\(peerURL != "missing")")
        guard pendingRequest == nil else {
            Task { await respondError(request, code: -32002, message: "Another X3 request is pending") }
            return
        }
        guard Self.supportedMethods.contains(request.method), request.chainId.namespace == "eip155" else {
            Task { await respondError(request, code: -32601, message: "Method not supported by X3") }
            return
        }
        if ["wallet_getCallsStatus", "wallet_showCallsStatus", "wallet_getCapabilities"].contains(request.method) {
            Task { await handleInformationalRequest(request) }
            return
        }
        pendingRequest = request
        pendingMethod = request.method
        status = "Preparing signing request"
        Task {
            do {
                let prepared = try await prepare(request)
                if let pendingBatch, request.method == "wallet_sendCalls" {
                    requestSummary = "\(pendingBatch.transactions.count) calls on chain \(request.chainId.reference)"
                    status = "Review batch call 1 of \(pendingBatch.transactions.count) on X3"
                } else {
                    requestSummary = "\(request.method) on chain \(request.chainId.reference)"
                    status = "Review signing request on X3"
                }
                signingHandler(prepared)
            } catch {
                pendingRequest = nil
                pendingMethod = nil
                pendingBatch = nil
                status = error.localizedDescription
                let code = (error as? WalletCallBatchError)?.code ?? -32602
                await respondError(request, code: code, message: error.localizedDescription)
            }
        }
    }

    private func prepare(_ request: Request) async throws -> PreparedWalletRequest {
        if request.method == "wallet_sendCalls" {
            return .transaction(try await prepareCallBatch(request))
        }
        if request.method == "eth_signTypedData_v4" {
            return .signature(try prepareTypedData(request))
        }
        if request.method == "eth_sign" {
            return .signature(try prepareMessage(request, ethSign: true))
        }
        if request.method == "personal_sign" {
            return .signature(try prepareMessage(request, ethSign: false))
        }
        return .transaction(try await prepareTransaction(request))
    }

    private func prepareTypedData(_ request: Request) throws -> PreparedEvmSignature {
        guard let address = walletAddress, let params = request.params.value as? [Any], params.count == 2,
              params.contains(where: { ($0 as? String)?.caseInsensitiveCompare(address) == .orderedSame }),
              let typedValue = params.first(where: { value in
                  guard let text = value as? String else { return true }
                  return text.caseInsensitiveCompare(address) != .orderedSame
              }),
              let session = WalletKit.instance.getSessions().first(where: { $0.topic == request.topic }),
              !session.peer.url.isEmpty else { throw WalletConnectBridgeError.invalidTransaction }
        return try EvmSignRequestEncoder.typedData(
            typedValue, chainID: request.chainId.reference, address: address, origin: session.peer.url
        )
    }

    private func prepareMessage(_ request: Request, ethSign: Bool) throws -> PreparedEvmSignature {
        guard let address = walletAddress else { throw WalletConnectBridgeError.signerUnavailable }
        guard let params = request.params.value as? [Any], params.count == 2 else {
            throw WalletConnectBridgeError.invalidParameters
        }
        guard params.contains(where: { ($0 as? String)?.caseInsensitiveCompare(address) == .orderedSame }) else {
            throw WalletConnectBridgeError.signerMismatch
        }
        guard let message = params.first(where: {
            ($0 as? String)?.caseInsensitiveCompare(address) != .orderedSame
        }) else { throw WalletConnectBridgeError.invalidParameters }
        guard let session = WalletKit.instance.getSessions().first(where: { $0.topic == request.topic }),
              !session.peer.url.isEmpty else { throw WalletConnectBridgeError.missingPeerMetadata }
        if ethSign {
            return try EvmSignRequestEncoder.ethSign(
                messageValue: message, chainID: request.chainId.reference, address: address, origin: session.peer.url
            )
        }
        return try EvmSignRequestEncoder.personalMessage(
            messageValue: message, chainID: request.chainId.reference, address: address, origin: session.peer.url
        )
    }

    private func prepareTransaction(_ request: Request) async throws -> PreparedEvmTransaction {
        guard let address = walletAddress,
              let params = request.params.value as? [Any],
              let transaction = params.first as? [String: Any],
              let from = transaction["from"] as? String,
              from.caseInsensitiveCompare(address) == .orderedSame,
              let recipient = transaction["to"] as? String else {
            throw WalletConnectBridgeError.invalidTransaction
        }
        if let type = transaction["type"] as? String, type != "0x2" {
            throw WalletConnectBridgeError.legacyTransaction
        }
        let chainID = request.chainId.reference
        let customRPC = customRPCProvider()
        let rpc = ReownRPCClient(
            projectID: projectID,
            customURL: customRPC.0,
            customChainID: customRPC.1
        )
        let nonce = if let supplied = try string(transaction, "nonce") {
            supplied
        } else {
            try await rpc.call(chainID: chainID, method: "eth_getTransactionCount",
                               params: [address, "pending"])
        }
        let gas = if let supplied = try string(transaction, "gas") {
            supplied
        } else {
            try await rpc.call(chainID: chainID, method: "eth_estimateGas", params: [transaction])
        }
        let priority = if let supplied = try string(transaction, "maxPriorityFeePerGas") {
            supplied
        } else {
            try await rpc.call(chainID: chainID, method: "eth_maxPriorityFeePerGas", params: [])
        }
        let maximum = if let supplied = try string(transaction, "maxFeePerGas") {
            supplied
        } else {
            try await rpc.call(chainID: chainID, method: "eth_gasPrice", params: [])
        }
        return try EvmTransactionEncoder.prepareHex(
            chainID: chainID,
            nonce: nonce,
            recipient: recipient,
            value: transaction["value"] as? String ?? "0x0",
            gasLimit: gas,
            maxPriorityFee: priority,
            maxFee: maximum,
            dataHex: transaction["data"] as? String ?? transaction["input"] as? String ?? "0x",
            accessList: transaction["accessList"]
        )
    }

    private func prepareCallBatch(_ request: Request) async throws -> PreparedEvmTransaction {
        guard let address = walletAddress else { throw WalletConnectBridgeError.signerUnavailable }
        let batch = try WalletCallBatchRequest.parse(
            request.params.value, expectedChainID: request.chainId.reference, expectedAddress: address
        )
        pruneBatchRecords()
        let id = try batch.id ?? Self.randomBatchID()
        if batchRecords[id] != nil || pendingBatch?.id == id { throw WalletCallBatchError.duplicateID }

        let customRPC = customRPCProvider()
        let rpc = ReownRPCClient(
            projectID: projectID, customURL: customRPC.0, customChainID: customRPC.1
        )
        let transactions = try await EvmTransactionService(rpc: rpc).prepareBatch(
            chainID: batch.chainIDDecimal, sender: address, calls: batch.calls
        )
        pendingBatch = PendingCallBatch(
            id: id, version: batch.version, chainIDHex: batch.chainIDHex,
            chainIDDecimal: batch.chainIDDecimal, transactions: transactions, rpc: rpc
        )
        status = "Review batch call 1 of \(transactions.count) on X3"
        return transactions[0]
    }

    private func completeBatchSigning(_ signingResult: WalletSigningResult, request: Request) {
        guard case .transaction(let rawTransaction) = signingResult, var batch = pendingBatch else {
            failSigning("The X3 returned an invalid wallet call signature")
            return
        }
        batch.signedTransactions.append(rawTransaction)
        let nextIndex = batch.signedTransactions.count
        pendingBatch = batch
        if nextIndex < batch.transactions.count {
            status = "Review batch call \(nextIndex + 1) of \(batch.transactions.count) on X3"
            signingHandler(.transaction(batch.transactions[nextIndex]))
            return
        }
        status = "Submitting \(batch.transactions.count) approved calls"
        Task { await submitApprovedBatch(request) }
    }

    private func submitApprovedBatch(_ request: Request) async {
        guard let batch = pendingBatch,
              batch.signedTransactions.count == batch.transactions.count else {
            failSigning("The wallet call batch was incomplete")
            return
        }
        var hashes: [String] = []
        hashes.reserveCapacity(batch.signedTransactions.count)
        var submissionError: Error?
        for rawTransaction in batch.signedTransactions {
            let rawHex = "0x" + rawTransaction.map { String(format: "%02x", $0) }.joined()
            do {
                let hash = try await batch.rpc.call(
                    chainID: batch.chainIDDecimal, method: "eth_sendRawTransaction", params: [rawHex]
                )
                hashes.append(hash)
            } catch {
                submissionError = error
                break
            }
        }

        pendingBatch = nil
        pendingRequest = nil
        pendingMethod = nil
        if hashes.isEmpty, let submissionError {
            status = submissionError.localizedDescription
            await respondError(request, code: -32603, message: submissionError.localizedDescription)
            return
        }
        batchRecords[batch.id] = CallBatchRecord(
            version: batch.version,
            chainIDHex: batch.chainIDHex,
            chainIDDecimal: batch.chainIDDecimal,
            transactionHashes: hashes,
            submissionFailed: submissionError != nil,
            createdAt: Date(),
            rpc: batch.rpc
        )
        status = submissionError == nil
            ? "Wallet call batch submitted"
            : "Wallet call batch partially submitted"
        await respondResult(request, value: ["id": batch.id])
    }

    private func handleInformationalRequest(_ request: Request) async {
        do {
            switch request.method {
            case "wallet_getCapabilities":
                try await respondCapabilities(request)
            case "wallet_getCallsStatus":
                try await respondCallsStatus(request)
            case "wallet_showCallsStatus":
                try await respondShowCallsStatus(request)
            default:
                await respondError(request, code: -32601, message: "Method not supported by X3")
            }
        } catch {
            let code = (error as? WalletCallBatchError)?.code ?? -32603
            await respondError(request, code: code, message: error.localizedDescription)
        }
    }

    private func respondCapabilities(_ request: Request) async throws {
        guard let address = walletAddress,
              let params = request.params.value as? [Any], !params.isEmpty, params.count <= 2,
              let requestedAddress = params[0] as? String else {
            throw WalletCallBatchError.invalidParameters
        }
        guard requestedAddress.caseInsensitiveCompare(address) == .orderedSame else {
            throw WalletCallBatchError.unauthorized
        }
        let requestedChains: [String]
        if params.count == 2 {
            guard let chains = params[1] as? [String] else {
                throw WalletCallBatchError.invalidParameters
            }
            requestedChains = chains
        } else {
            guard let chain = UInt64(request.chainId.reference) else {
                throw WalletCallBatchError.invalidParameters
            }
            requestedChains = ["0x" + String(chain, radix: 16)]
        }
        var result: [String: Any] = [:]
        for requestedChain in requestedChains {
            guard requestedChain.hasPrefix("0x") else {
                throw WalletCallBatchError.invalidParameters
            }
            let digits = requestedChain.dropFirst(2)
            guard !digits.isEmpty, digits.allSatisfy(\.isHexDigit),
                  digits.count == 1 || digits.first != "0", let chain = UInt64(digits, radix: 16) else {
                throw WalletCallBatchError.invalidParameters
            }
            let chainHex = "0x" + String(chain, radix: 16)
            result[chainHex] = ["atomic": ["status": "unsupported"]]
        }
        await respondResult(request, value: result)
    }

    private func respondCallsStatus(_ request: Request) async throws {
        let id = try batchID(from: request.params.value)
        pruneBatchRecords()
        guard let record = batchRecords[id], record.chainIDDecimal == request.chainId.reference else {
            throw WalletCallBatchError.unknownID
        }
        var receipts: [[String: Any]] = []
        receipts.reserveCapacity(record.transactionHashes.count)
        for transactionHash in record.transactionHashes {
            let raw = try await record.rpc.callJSON(
                chainID: record.chainIDDecimal, method: "eth_getTransactionReceipt", params: [transactionHash]
            )
            if raw is NSNull { continue }
            receipts.append(try Self.callReceipt(raw))
        }

        let mined = receipts.count == record.transactionHashes.count
        let successful = receipts.filter { ($0["status"] as? String) == "0x1" }.count
        let batchStatus: Int
        if !mined {
            batchStatus = 100
        } else if record.submissionFailed {
            batchStatus = 600
        } else if successful == receipts.count {
            batchStatus = 200
        } else if successful == 0 {
            batchStatus = 500
        } else {
            batchStatus = 600
        }
        var result: [String: Any] = [
            "version": record.version,
            "id": id,
            "chainId": record.chainIDHex,
            "status": batchStatus,
            "atomic": false,
        ]
        if !receipts.isEmpty { result["receipts"] = receipts }
        await respondResult(request, value: result)
    }

    private func respondShowCallsStatus(_ request: Request) async throws {
        let id = try batchID(from: request.params.value)
        pruneBatchRecords()
        guard let record = batchRecords[id], record.chainIDDecimal == request.chainId.reference else {
            throw WalletCallBatchError.unknownID
        }
        status = "Batch \(id.prefix(10))… has \(record.transactionHashes.count) submitted transaction(s)"
        await respondResult(request, value: NSNull())
    }

    private func batchID(from value: Any) throws -> String {
        if let id = value as? String { return id }
        guard let params = value as? [Any], params.count == 1, let id = params[0] as? String else {
            throw WalletCallBatchError.invalidParameters
        }
        return id
    }

    private func pruneBatchRecords() {
        let cutoff = Date().addingTimeInterval(-24 * 60 * 60)
        batchRecords = batchRecords.filter { $0.value.createdAt >= cutoff }
    }

    private static func randomBatchID() throws -> String {
        var bytes = [UInt8](repeating: 0, count: 64)
        let byteCount = bytes.count
        let status = bytes.withUnsafeMutableBytes { buffer in
            SecRandomCopyBytes(kSecRandomDefault, byteCount, buffer.baseAddress!)
        }
        guard status == errSecSuccess else {
            throw ReownRPCError.invalidResponse
        }
        return "0x" + bytes.map { String(format: "%02x", $0) }.joined()
    }

    private static func callReceipt(_ value: Any) throws -> [String: Any] {
        guard let receipt = value as? [String: Any],
              let status = receipt["status"] as? String,
              let blockHash = receipt["blockHash"] as? String,
              let blockNumber = receipt["blockNumber"] as? String,
              let gasUsed = receipt["gasUsed"] as? String,
              let transactionHash = receipt["transactionHash"] as? String,
              let rawLogs = receipt["logs"] as? [Any] else {
            throw ReownRPCError.invalidResponse
        }
        let logs = try rawLogs.map { rawLog -> [String: Any] in
            guard let log = rawLog as? [String: Any],
                  let address = log["address"] as? String,
                  let data = log["data"] as? String,
                  let topics = log["topics"] as? [String] else {
                throw ReownRPCError.invalidResponse
            }
            return ["address": address, "data": data, "topics": topics]
        }
        return [
            "logs": logs,
            "status": status,
            "blockHash": blockHash,
            "blockNumber": blockNumber,
            "gasUsed": gasUsed,
            "transactionHash": transactionHash,
        ]
    }

    private func string(_ dictionary: [String: Any], _ key: String) throws -> String? {
        guard let value = dictionary[key] else { return nil }
        guard let text = value as? String else { throw WalletConnectBridgeError.invalidTransaction }
        return text
    }

    private func respondError(_ request: Request, code: Int, message: String) async {
        try? await WalletKit.instance.respond(
            topic: request.topic,
            requestId: request.id,
            response: .error(JSONRPCError(code: code, message: message))
        )
    }

    private func respondResult(_ request: Request, value: Any) async {
        try? await WalletKit.instance.respond(
            topic: request.topic,
            requestId: request.id,
            response: .response(AnyCodable(any: value))
        )
    }
}

enum WalletConnectBridgeError: LocalizedError {
    case unsupportedProposal
    case invalidTransaction
    case invalidParameters
    case signerUnavailable
    case signerMismatch
    case missingPeerMetadata
    case legacyTransaction

    var errorDescription: String? {
        switch self {
        case .unsupportedProposal: "The dapp requires methods or chains this X3 build cannot safely handle"
        case .invalidTransaction: "The dapp supplied an invalid or mismatched transaction"
        case .invalidParameters: "The dapp supplied malformed signing parameters"
        case .signerUnavailable: "The X3 signer is not connected"
        case .signerMismatch: "The requested account does not match the connected X3"
        case .missingPeerMetadata: "The restored WalletConnect session has no trusted dapp origin"
        case .legacyTransaction: "The X3 currently supports EIP-1559 transactions only"
        }
    }
}
