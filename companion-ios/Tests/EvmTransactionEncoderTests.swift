import XCTest
@testable import X3Companion

final class EvmTransactionEncoderTests: XCTestCase {
    func testCustomRPCURLValidationRequiresHTTPS() {
        XCTAssertEqual(
            RPCSettings.validatedURL("  https://rpc.example.com/key?token=abc  ")?.absoluteString,
            "https://rpc.example.com/key?token=abc"
        )
        XCTAssertNil(RPCSettings.validatedURL("http://rpc.example.com"))
        XCTAssertNil(RPCSettings.validatedURL("https://"))
        XCTAssertNil(RPCSettings.validatedURL("https://rpc.example.com/#fragment"))
    }

    func testCustomRPCIsUsedOnlyForItsDetectedChain() throws {
        let url = URL(string: "https://rpc.example.com/key")!
        let client = ReownRPCClient(projectID: "ignored", customURL: url, customChainID: "10")
        XCTAssertEqual(try client.endpointURL(chainID: "10"), url)
        XCTAssertThrowsError(try client.endpointURL(chainID: "1")) { error in
            XCTAssertEqual(error.localizedDescription, "This RPC serves chain 10, not chain 1")
        }
    }

    func testWalletConnectRPCIncludesRequestedChainAndProject() throws {
        let url = try ReownRPCClient(projectID: "project-123").endpointURL(chainID: "8453")
        let components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        XCTAssertEqual(components?.queryItems?.first(where: { $0.name == "chainId" })?.value, "eip155:8453")
        XCTAssertEqual(components?.queryItems?.first(where: { $0.name == "projectId" })?.value, "project-123")
    }

    func testReceiveQRCodeUsesEthereumURI() {
        XCTAssertEqual(
            ReceiveView.qrPayload(for: "0x1111111111111111111111111111111111111111"),
            "ethereum:0x1111111111111111111111111111111111111111"
        )
        XCTAssertNil(ReceiveView.qrPayload(for: "Not connected"))
    }

    func testReceiveQRCodeIsBackedByRenderedPixels() {
        let image = ReceiveView.qrImage(for: "0x1111111111111111111111111111111111111111")
        XCTAssertNotNil(image?.cgImage)
        XCTAssertGreaterThan(image?.cgImage?.width ?? 0, 200)
        XCTAssertGreaterThan(image?.cgImage?.height ?? 0, 200)
    }

    func testCanonicalOptimismTransactionMatchesFirmwareVector() throws {
        let prepared = try EvmTransactionEncoder.prepare(
            chainID: "10", nonce: "1", recipient: "0x1111111111111111111111111111111111111111",
            amount: "0.01", gasLimit: "21000", maxPriorityFeeGwei: "1", maxFeeGwei: "2",
            dataHex: "0x", requestID: 7
        )
        XCTAssertEqual(prepared.unsignedTransaction.hex,
                       "02ee0a01843b9aca008477359400825208941111111111111111111111111111111111111111872386f26fc1000080c0")
        XCTAssertEqual(prepared.request.count, EvmSignRequestEncoder.wireSize)
        XCTAssertEqual(prepared.request[5], 0)
    }

    func testSignatureCompletesTypedEnvelope() throws {
        let prepared = try EvmTransactionEncoder.prepare(
            chainID: "10", nonce: "1", recipient: "0x1111111111111111111111111111111111111111",
            amount: "0.01", gasLimit: "21000", maxPriorityFeeGwei: "1", maxFeeGwei: "2", dataHex: "0x"
        )
        var signature = Data(repeating: 0, count: 65)
        signature[31] = 1
        signature[63] = 2
        signature[64] = 1
        let signed = try prepared.signedTransaction(signature: signature)
        XCTAssertEqual(signed.last, 2)
        XCTAssertEqual(signed.first, 2)
    }

    func testWalletConnectHexQuantitiesMatchManualTransaction() throws {
        let prepared = try EvmTransactionEncoder.prepareHex(
            chainID: "10", nonce: "0x1", recipient: "0x1111111111111111111111111111111111111111",
            value: "0x2386f26fc10000", gasLimit: "0x5208", maxPriorityFee: "0x3b9aca00",
            maxFee: "0x77359400", dataHex: "0x", requestID: 7
        )
        XCTAssertEqual(prepared.unsignedTransaction.hex,
                       "02ee0a01843b9aca008477359400825208941111111111111111111111111111111111111111872386f26fc1000080c0")
    }

    func testWalletConnectEncodesTypeTwoAccessList() throws {
        let prepared = try EvmTransactionEncoder.prepareHex(
            chainID: "10", nonce: "0x1", recipient: "0x1111111111111111111111111111111111111111",
            value: "0x2386f26fc10000", gasLimit: "0x5208", maxPriorityFee: "0x3b9aca00",
            maxFee: "0x77359400", dataHex: "0x",
            accessList: [[
                "address": "0x2222222222222222222222222222222222222222",
                "storageKeys": ["0x" + String(repeating: "33", count: 32)],
            ]], requestID: 7
        )
        XCTAssertEqual(
            prepared.unsignedTransaction.hex,
                "02f8670a01843b9aca008477359400825208941111111111111111111111111111111111111111872386f26fc1000080" +
                    "f838f7942222222222222222222222222222222222222222e1a0333333333333333333333333333333333333333333" +
                    "3333333333333333333333"
        )
    }

    func testWalletConnectRejectsMalformedAccessList() throws {
        XCTAssertThrowsError(try EvmTransactionEncoder.prepareHex(
            chainID: "10", nonce: "0x1", recipient: "0x1111111111111111111111111111111111111111",
            value: "0x0", gasLimit: "0x5208", maxPriorityFee: "0x3b9aca00", maxFee: "0x77359400",
            dataHex: "0x", accessList: [["address": "0x1234", "storageKeys": []]]
        )) { error in
            XCTAssertEqual(error.localizedDescription, "Transaction access list is malformed")
        }
    }

    func testManualTransactionAcceptsFractionalGweiFees() throws {
        let manual = try EvmTransactionEncoder.prepare(
            chainID: "10", nonce: "1", recipient: "0x1111111111111111111111111111111111111111",
            amount: "0.01", gasLimit: "21000", maxPriorityFeeGwei: "1.5", maxFeeGwei: "2.25",
            dataHex: "0x", requestID: 7
        )
        let hexadecimal = try EvmTransactionEncoder.prepareHex(
            chainID: "10", nonce: "0x1", recipient: "0x1111111111111111111111111111111111111111",
            value: "0x2386f26fc10000", gasLimit: "0x5208", maxPriorityFee: "0x59682f00",
            maxFee: "0x861c4680", dataHex: "0x", requestID: 7
        )
        XCTAssertEqual(manual.unsignedTransaction, hexadecimal.unsignedTransaction)
    }

    func testAutomaticPreparationUsesPendingNonceAndLiveNetworkValues() async throws {
        let rpc = StubEvmRPC(responses: [
            "eth_getTransactionCount": "0x7",
            "eth_estimateGas": "0x5208",
            "eth_maxPriorityFeePerGas": "0x59682f00",
            "eth_gasPrice": "0x861c4680",
        ])
        let result = try await EvmTransactionService(rpc: rpc).prepare(
            chainID: "10",
            sender: "0x2222222222222222222222222222222222222222",
            recipient: "0x1111111111111111111111111111111111111111",
            amount: "0.01",
            dataHex: "0x"
        )

        XCTAssertEqual(result.nonce, "7")
        XCTAssertEqual(result.gasLimit, "21000")
        XCTAssertEqual(result.priorityFeeGwei, "1.5")
        XCTAssertEqual(result.maxFeeGwei, "2.25")
        XCTAssertEqual(rpc.params(for: "eth_getTransactionCount")?[1] as? String, "pending")
        let estimated = rpc.params(for: "eth_estimateGas")?.first as? [String: String]
        XCTAssertEqual(estimated?["from"], "0x2222222222222222222222222222222222222222")
        XCTAssertEqual(estimated?["value"], "0x2386f26fc10000")
    }

    func testAutomaticPreparationReportsInsufficientFundsClearly() async {
        let rpc = StubEvmRPC(responses: [
            "eth_getTransactionCount": "0x0",
            "eth_estimateGas": StubEvmRPC.outOfFunds,
            "eth_maxPriorityFeePerGas": "0xf4240",
            "eth_gasPrice": "0x3d547eff",
        ])

        do {
            _ = try await EvmTransactionService(rpc: rpc).prepare(
                chainID: "11155111",
                sender: "0x2222222222222222222222222222222222222222",
                recipient: "0x1111111111111111111111111111111111111111",
                amount: "0.01",
                dataHex: "0x"
            )
            XCTFail("Expected insufficient funds")
        } catch let error as EvmTransactionServiceError {
            XCTAssertEqual(error.errorDescription,
                           "This wallet needs enough native token to cover the transfer and network fee")
        } catch {
            XCTFail("Unexpected error: \(error)")
        }
    }

    func testWalletConnectCryptoProviderUsesEthereumKeccak() {
        let provider = X3CryptoProvider()
        XCTAssertEqual(provider.keccak256(Data()).hex,
                       "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470")
        XCTAssertEqual(provider.keccak256(Data("abc".utf8)).hex,
                       "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45")
    }

    func testWalletConnectPairingQRPayloads() {
        let raw = "wc:abc123@2?relay-protocol=irn&symKey=feed"
        XCTAssertEqual(WalletConnectManager.pairingURI(from: raw), raw)
        var wrapped = URLComponents(string: "https://example.test/connect")!
        wrapped.queryItems = [URLQueryItem(name: "uri", value: raw)]
        XCTAssertEqual(WalletConnectManager.pairingURI(from: wrapped.url!.absoluteString), raw)
        XCTAssertNil(WalletConnectManager.pairingURI(from: "https://example.test/not-walletconnect"))
    }

    func testFlatPermitTypedDataUsesTypedSigningRequest() throws {
        let typedData: [String: Any] = [
            "types": [
                "EIP712Domain": [
                    ["name": "name", "type": "string"],
                    ["name": "version", "type": "string"],
                    ["name": "chainId", "type": "uint256"],
                    ["name": "verifyingContract", "type": "address"],
                ],
                "Permit": [
                    ["name": "owner", "type": "address"],
                    ["name": "spender", "type": "address"],
                    ["name": "value", "type": "uint256"],
                    ["name": "nonce", "type": "uint256"],
                    ["name": "deadline", "type": "uint256"],
                ],
            ],
            "primaryType": "Permit",
            "domain": [
                "name": "USD Coin", "version": "2", "chainId": "1",
                "verifyingContract": "0x0000000000000000000000000000000000000001",
            ],
            "message": [
                "owner": "0x1111111111111111111111111111111111111111",
                "spender": "0x2222222222222222222222222222222222222222",
                "value": "1000000", "nonce": "7", "deadline": "9999999999",
            ],
        ]
        let prepared = try EvmSignRequestEncoder.typedData(
            typedData, chainID: "1", address: "0x1111111111111111111111111111111111111111",
            origin: "https://example.com", requestID: 9
        )
        XCTAssertEqual(prepared.request.count, EvmSignRequestEncoder.wireSize)
        XCTAssertEqual(prepared.request[5], 1)
    }

    func testTypedDataRejectsNestedTypes() {
        let typedData: [String: Any] = [
            "types": [
                "EIP712Domain": [["name": "chainId", "type": "uint256"]],
                "Order": [["name": "maker", "type": "Person"]],
                "Person": [["name": "wallet", "type": "address"]],
            ],
            "primaryType": "Order", "domain": ["chainId": "1"],
            "message": ["maker": ["wallet": "0x1111111111111111111111111111111111111111"]],
        ]
        XCTAssertThrowsError(try EvmSignRequestEncoder.typedData(
            typedData, chainID: "1", address: "0x1111111111111111111111111111111111111111",
            origin: "https://example.com"
        ))
    }

    func testSiweMessageUsesPersonalSigningRequest() throws {
        let address = "0x1111111111111111111111111111111111111111"
        let message = """
        example.com wants you to sign in with your Ethereum account:
        \(address)

        Sign in to the app.

        URI: https://example.com/login
        Version: 1
        Chain ID: 1
        Nonce: abcdef12
        Issued At: 2026-08-26T12:00:00Z
        """
        let prepared = try EvmSignRequestEncoder.siwe(
            messageValue: message, chainID: "1", address: address, origin: "https://example.com", requestID: 10
        )
        XCTAssertEqual(prepared.request[5], 2)
    }

    func testGenericPersonalMessageUsesPersonalSigningRequest() throws {
        let prepared = try EvmSignRequestEncoder.personalMessage(
            messageValue: "Hello from WalletConnect", chainID: "11155111",
            address: "0x1111111111111111111111111111111111111111", origin: "https://example.com", requestID: 11
        )
        XCTAssertEqual(prepared.request[5], 2)
        XCTAssertEqual(prepared.request.count, EvmSignRequestEncoder.wireSize)
    }
}

private final class StubEvmRPC: EvmRPCRequesting, @unchecked Sendable {
    static let outOfFunds = "__out_of_funds__"
    private let lock = NSLock()
    private let responses: [String: String]
    private var calls: [(String, [Any])] = []

    init(responses: [String: String]) {
        self.responses = responses
    }

    func call(chainID _: String, method: String, params: [Any]) async throws -> String {
        lock.withLock { calls.append((method, params)) }
        guard let response = responses[method] else { throw ReownRPCError.invalidResponse }
        if response == Self.outOfFunds { throw ReownRPCError.remote("EVM error: OutOfFunds") }
        return response
    }

    func params(for method: String) -> [Any]? {
        lock.withLock { calls.first(where: { $0.0 == method })?.1 }
    }
}

private extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }
}
