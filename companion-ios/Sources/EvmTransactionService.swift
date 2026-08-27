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

enum EvmTransactionServiceError: LocalizedError {
    case missingProjectID
    case walletUnavailable
    case insufficientFunds
    case invalidRPCQuantity(String)

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
        }
    }
}

struct EvmTransactionService {
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
