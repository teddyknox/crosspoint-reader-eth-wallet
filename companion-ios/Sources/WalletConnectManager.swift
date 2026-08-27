import Combine
import Foundation
import ReownWalletKit
import WalletConnectNetworking
import WalletConnectSign

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
        "eth_signTransaction", "eth_sendTransaction", "eth_signTypedData_v4", "personal_sign",
    ]
    private static let supportedEvents: Set<String> = ["accountsChanged", "chainChanged"]

    private var configured = false
    private var walletAddress: String?
    private var pendingProposal: Session.Proposal?
    private var pendingRequest: Request?
    private var pendingMethod: String?
    private var cancellables = Set<AnyCancellable>()
    private let customRPCProvider: () -> (URL?, String?)
    private let signingHandler: SigningHandler

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
        pendingRequest = request
        pendingMethod = request.method
        status = "Preparing signing request"
        Task {
            do {
                let prepared = try await prepare(request)
                requestSummary = "\(request.method) on chain \(request.chainId.reference)"
                status = "Review signing request on X3"
                signingHandler(prepared)
            } catch {
                pendingRequest = nil
                pendingMethod = nil
                status = error.localizedDescription
                await respondError(request, code: -32602, message: error.localizedDescription)
            }
        }
    }

    private func prepare(_ request: Request) async throws -> PreparedWalletRequest {
        if request.method == "eth_signTypedData_v4" {
            return .signature(try prepareTypedData(request))
        }
        if request.method == "personal_sign" {
            return .signature(try preparePersonalMessage(request))
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

    private func preparePersonalMessage(_ request: Request) throws -> PreparedEvmSignature {
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
