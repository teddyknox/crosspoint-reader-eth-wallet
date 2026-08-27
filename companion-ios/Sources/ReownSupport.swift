import Foundation
import WalletConnectRelay
import WalletConnectSigner

private final class NativeWebSocket: NSObject, WebSocketConnecting, URLSessionWebSocketDelegate {
    var onConnect: (() -> Void)?
    var onDisconnect: ((Error?) -> Void)?
    var onText: ((String) -> Void)?
    var request: URLRequest

    private(set) var isConnected = false
    private var task: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .default, delegate: self,
                                          delegateQueue: nil)

    init(url: URL) {
        request = URLRequest(url: url)
    }

    func connect() {
        guard task == nil else { return }
        let task = session.webSocketTask(with: request)
        self.task = task
        task.resume()
    }

    func disconnect() {
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
        if isConnected {
            isConnected = false
            onDisconnect?(nil)
        }
    }

    func write(string: String, completion: (() -> Void)?) {
        task?.send(.string(string)) { error in
            if let error {
                self.finish(error)
            } else {
                completion?()
            }
        }
    }

    func urlSession(_: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol _: String?) {
        isConnected = true
        onConnect?()
        receive(from: webSocketTask)
    }

    func urlSession(_: URLSession, webSocketTask _: URLSessionWebSocketTask,
                    didCloseWith _: URLSessionWebSocketTask.CloseCode, reason _: Data?) {
        finish(nil)
    }

    func urlSession(_: URLSession, task _: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { finish(error) }
    }

    private func receive(from task: URLSessionWebSocketTask) {
        task.receive { result in
            switch result {
            case .success(.string(let text)):
                self.onText?(text)
                self.receive(from: task)
            case .success(.data(let data)):
                if let text = String(data: data, encoding: .utf8) { self.onText?(text) }
                self.receive(from: task)
            case .failure(let error):
                self.finish(error)
            @unknown default:
                self.finish(ReownSupportError.unsupportedWebSocketMessage)
            }
        }
    }

    private func finish(_ error: Error?) {
        let wasActive = task != nil || isConnected
        task = nil
        isConnected = false
        if wasActive { onDisconnect?(error) }
    }
}

struct X3SocketFactory: WebSocketFactory {
    func create(with url: URL) -> WebSocketConnecting {
        NativeWebSocket(url: url)
    }
}

/// WalletKit requires these primitives for optional verification features. Transaction
/// hashing/signing still happens only on the X3; public-key recovery is deliberately absent.
struct X3CryptoProvider: CryptoProvider {
    func recoverPubKey(signature _: EthereumSignature, message _: Data) throws -> Data {
        throw ReownSupportError.publicKeyRecoveryUnsupported
    }

    func keccak256(_ data: Data) -> Data {
        Keccak256.hash(data)
    }
}

private enum ReownSupportError: LocalizedError {
    case publicKeyRecoveryUnsupported
    case unsupportedWebSocketMessage

    var errorDescription: String? {
        switch self {
        case .publicKeyRecoveryUnsupported:
            "WalletConnect message-signature recovery is not supported"
        case .unsupportedWebSocketMessage:
            "WalletConnect relay returned an unsupported WebSocket message"
        }
    }
}

private enum Keccak256 {
    private static let rate = 136
    private static let roundConstants: [UInt64] = [
        0x0000000000000001, 0x0000000000008082, 0x800000000000808A,
        0x8000000080008000, 0x000000000000808B, 0x0000000080000001,
        0x8000000080008081, 0x8000000000008009, 0x000000000000008A,
        0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
        0x000000008000808B, 0x800000000000008B, 0x8000000000008089,
        0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
        0x000000000000800A, 0x800000008000000A, 0x8000000080008081,
        0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
    ]
    private static let rotations = [
         0,  1, 62, 28, 27,
        36, 44,  6, 55, 20,
         3, 10, 43, 25, 39,
        41, 45, 15, 21,  8,
        18,  2, 61, 56, 14,
    ]

    static func hash(_ input: Data) -> Data {
        var bytes = [UInt8](input)
        bytes.append(0x01)
        bytes.append(contentsOf: repeatElement(0, count: (rate - bytes.count % rate) % rate))
        bytes[bytes.count - 1] |= 0x80

        var state = [UInt64](repeating: 0, count: 25)
        for offset in stride(from: 0, to: bytes.count, by: rate) {
            for index in 0..<rate {
                state[index / 8] ^= UInt64(bytes[offset + index]) << UInt64(8 * (index % 8))
            }
            permute(&state)
        }

        var digest = Data(capacity: 32)
        for index in 0..<32 {
            digest.append(UInt8(truncatingIfNeeded: state[index / 8] >> UInt64(8 * (index % 8))))
        }
        return digest
    }

    private static func permute(_ state: inout [UInt64]) {
        for constant in roundConstants {
            var column = [UInt64](repeating: 0, count: 5)
            for x in 0..<5 {
                column[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20]
            }
            for x in 0..<5 {
                let delta = column[(x + 4) % 5] ^ column[(x + 1) % 5].rotatedLeft(1)
                for y in 0..<5 { state[x + 5 * y] ^= delta }
            }

            var moved = [UInt64](repeating: 0, count: 25)
            for x in 0..<5 {
                for y in 0..<5 {
                    moved[y + 5 * ((2 * x + 3 * y) % 5)] =
                        state[x + 5 * y].rotatedLeft(rotations[x + 5 * y])
                }
            }
            for x in 0..<5 {
                for y in 0..<5 {
                    state[x + 5 * y] = moved[x + 5 * y] ^
                        (~moved[(x + 1) % 5 + 5 * y] & moved[(x + 2) % 5 + 5 * y])
                }
            }
            state[0] ^= constant
        }
    }
}

private extension UInt64 {
    func rotatedLeft(_ count: Int) -> UInt64 {
        guard count != 0 else { return self }
        return (self << UInt64(count)) | (self >> UInt64(64 - count))
    }
}
