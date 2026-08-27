import Combine
import Foundation
import Security

private let rpcKeychainService = "com.teddyknox.X3Companion.rpc"
private let rpcKeychainAccount = "ethereum-json-rpc"

enum RPCSettingsError: LocalizedError {
    case invalidURL
    case keychain(OSStatus)

    var errorDescription: String? {
        switch self {
        case .invalidURL:
            "Enter a valid HTTPS RPC URL"
        case .keychain(let status):
            "Could not save RPC setting (\(status))"
        }
    }
}

@MainActor
final class RPCSettings: ObservableObject {
    private struct StoredRPC: Codable {
        let url: URL
        let chainID: String
    }

    @Published private(set) var customURL: URL?
    @Published private(set) var customChainID: String?

    var label: String {
        customURL?.host ?? "WalletConnect"
    }

    init() {
        let stored = Self.storedRPC()
        customURL = stored?.url
        customChainID = stored?.chainID
    }

    func useCustom(_ url: URL, chainID: String) throws {
        let rpc = StoredRPC(url: url, chainID: chainID)
        try Self.store(rpc)
        customURL = url
        customChainID = chainID
    }

    func useWalletConnect() throws {
        try Self.store(nil)
        customURL = nil
        customChainID = nil
    }

    nonisolated static func validatedURL(_ text: String) -> URL? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let components = URLComponents(string: trimmed),
              components.scheme?.lowercased() == "https",
              components.host?.isEmpty == false,
              components.fragment == nil,
              let url = components.url else { return nil }
        return url
    }

    private nonisolated static var query: [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: rpcKeychainService,
            kSecAttrAccount as String: rpcKeychainAccount,
        ]
    }

    private nonisolated static func storedRPC() -> StoredRPC? {
        var lookup = query
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return try? JSONDecoder().decode(StoredRPC.self, from: data)
    }

    private nonisolated static func store(_ rpc: StoredRPC?) throws {
        guard let rpc else {
            let status = SecItemDelete(query as CFDictionary)
            guard status == errSecSuccess || status == errSecItemNotFound else {
                throw RPCSettingsError.keychain(status)
            }
            return
        }

        let data = try JSONEncoder().encode(rpc)
        let updateStatus = SecItemUpdate(
            query as CFDictionary,
            [kSecValueData as String: data] as CFDictionary
        )
        if updateStatus == errSecSuccess { return }
        guard updateStatus == errSecItemNotFound else { throw RPCSettingsError.keychain(updateStatus) }

        var item = query
        item[kSecValueData as String] = data
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let addStatus = SecItemAdd(item as CFDictionary, nil)
        guard addStatus == errSecSuccess else { throw RPCSettingsError.keychain(addStatus) }
    }
}
