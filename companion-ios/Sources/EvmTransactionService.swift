import Foundation

protocol EvmRPCRequesting: Sendable {
    func call(chainID: String, method: String, params: [Any]) async throws -> String
}

struct AutomaticEvmTransaction {
    let prepared: PreparedEvmTransaction
    let nonce: String
    let gasLimit: String
    let priorityFeeGwei: String
    let maxFeeGwei: String
}

struct EvmWalletCall {
    let recipient: String
    let value: String
    let data: String
}

enum EvmTransactionServiceError: LocalizedError {
    case missingProjectID
    case walletUnavailable
    case insufficientFunds
    case invalidRPCQuantity(String)
    case batchTooLarge
    case nonceOverflow

    var errorDescription: String? {
        switch self {
        case .missingProjectID:
            "WalletConnect project ID is required for automatic network values"
        case .walletUnavailable:
            "Open Ethereum Wallet on X3 before preparing a transaction"
        case .insufficientFunds:
            "This wallet needs enough native token to cover the transfer and network fee"
        case .invalidRPCQuantity(let field):
            "The chain RPC returned an invalid \(field)"
        case .batchTooLarge:
            "A wallet call batch must contain between 1 and 8 calls"
        case .nonceOverflow:
            "The wallet nonce is too large for this batch"
        }
    }
}

struct EvmTransactionService {
    static let maximumBatchCalls = 8
    let rpc: any EvmRPCRequesting

    func prepare(chainID: String, sender: String, recipient: String, amount: String,
                 dataHex: String) async throws -> AutomaticEvmTransaction {
        let transaction = try EvmTransactionEncoder.rpcTransaction(
            sender: sender,
            recipient: recipient,
            amount: amount,
            dataHex: dataHex
        )

        // The pending count includes transactions already submitted but not mined,
        // which prevents reusing a nonce while this wallet has work in flight.
        async let nonceRequest = rpc.call(
            chainID: chainID,
            method: "eth_getTransactionCount",
            params: [sender, "pending"]
        )
        async let gasRequest = rpc.call(
            chainID: chainID,
            method: "eth_estimateGas",
            params: [transaction]
        )
        async let priorityRequest = rpc.call(
            chainID: chainID,
            method: "eth_maxPriorityFeePerGas",
            params: []
        )
        async let gasPriceRequest = rpc.call(
            chainID: chainID,
            method: "eth_gasPrice",
            params: []
        )

        let responses: (String, String, String, String)
        do {
            responses = try await (nonceRequest, gasRequest, priorityRequest, gasPriceRequest)
        } catch {
            let message = error.localizedDescription.lowercased()
            if message.contains("outoffunds") || message.contains("insufficient funds") {
                throw EvmTransactionServiceError.insufficientFunds
            }
            throw error
        }
        let (nonceHex, gasHex, priorityHex, gasPriceHex) = responses
        let nonce = try quantity(nonceHex, field: "nonce")
        let gas = try quantity(gasHex, field: "gas estimate")
        let priority = try quantity(priorityHex, field: "priority fee")
        let gasPrice = try quantity(gasPriceHex, field: "maximum fee")
        let maximum = max(priority, gasPrice)
        let maximumHex = "0x" + String(maximum, radix: 16)

        let prepared = try EvmTransactionEncoder.prepareHex(
            chainID: chainID,
            nonce: nonceHex,
            recipient: recipient,
            value: transaction["value"]!,
            gasLimit: gasHex,
            maxPriorityFee: priorityHex,
            maxFee: maximumHex,
            dataHex: dataHex
        )
        return AutomaticEvmTransaction(
            prepared: prepared,
            nonce: String(nonce),
            gasLimit: String(gas),
            priorityFeeGwei: Self.gweiString(wei: priority),
            maxFeeGwei: Self.gweiString(wei: maximum)
        )
    }

    func prepareBatch(chainID: String, sender: String, calls: [EvmWalletCall]) async throws
        -> [PreparedEvmTransaction] {
        guard !calls.isEmpty, calls.count <= Self.maximumBatchCalls else {
            throw EvmTransactionServiceError.batchTooLarge
        }

        async let nonceRequest = rpc.call(
            chainID: chainID, method: "eth_getTransactionCount", params: [sender, "pending"]
        )
        async let priorityRequest = rpc.call(
            chainID: chainID, method: "eth_maxPriorityFeePerGas", params: []
        )
        async let gasPriceRequest = rpc.call(
            chainID: chainID, method: "eth_gasPrice", params: []
        )
        let (nonceHex, priorityHex, gasPriceHex) = try await (nonceRequest, priorityRequest, gasPriceRequest)
        let startingNonce = try quantity(nonceHex, field: "nonce")
        let priority = try quantity(priorityHex, field: "priority fee")
        let gasPrice = try quantity(gasPriceHex, field: "maximum fee")
        let maximumHex = "0x" + String(max(priority, gasPrice), radix: 16)
        let count = UInt8(calls.count)
        var prepared: [PreparedEvmTransaction] = []
        prepared.reserveCapacity(calls.count)

        for (index, call) in calls.enumerated() {
            guard UInt64(index) <= UInt64.max - startingNonce else {
                throw EvmTransactionServiceError.nonceOverflow
            }
            let transaction = try EvmTransactionEncoder.rpcTransactionHex(
                sender: sender, recipient: call.recipient, value: call.value, dataHex: call.data
            )
            let gasHex: String
            do {
                gasHex = try await rpc.call(chainID: chainID, method: "eth_estimateGas", params: [transaction])
            } catch {
                let message = error.localizedDescription.lowercased()
                if message.contains("outoffunds") || message.contains("insufficient funds") {
                    throw EvmTransactionServiceError.insufficientFunds
                }
                throw error
            }
            _ = try quantity(gasHex, field: "gas estimate")
            prepared.append(try EvmTransactionEncoder.prepareHex(
                chainID: chainID,
                nonce: "0x" + String(startingNonce + UInt64(index), radix: 16),
                recipient: call.recipient,
                value: call.value,
                gasLimit: gasHex,
                maxPriorityFee: priorityHex,
                maxFee: maximumHex,
                dataHex: call.data,
                batchPosition: UInt8(index + 1),
                batchCount: count
            ))
        }
        return prepared
    }

    private func quantity(_ value: String, field: String) throws -> UInt64 {
        guard value.hasPrefix("0x"), let quantity = UInt64(value.dropFirst(2), radix: 16) else {
            throw EvmTransactionServiceError.invalidRPCQuantity(field)
        }
        return quantity
    }

    private static func gweiString(wei: UInt64) -> String {
        let whole = wei / 1_000_000_000
        let remainder = wei % 1_000_000_000
        guard remainder > 0 else { return String(whole) }
        var fraction = String(format: "%09llu", remainder)
        while fraction.last == "0" { fraction.removeLast() }
        return "\(whole).\(fraction)"
    }
}
