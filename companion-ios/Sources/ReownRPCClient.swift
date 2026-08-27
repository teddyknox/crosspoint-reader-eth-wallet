import Foundation

enum ReownRPCError: LocalizedError {
    case invalidURL
    case invalidResponse
    case chainMismatch(expected: String, actual: String)
    case remote(String)

    var errorDescription: String? {
        switch self {
        case .invalidURL: "Enter a valid HTTPS RPC URL"
        case .invalidResponse: "The chain RPC returned an invalid response"
        case .chainMismatch(let expected, let actual):
            "This RPC serves chain \(expected), not chain \(actual)"
        case .remote(let message): message
        }
    }
}

struct ReownRPCClient: EvmRPCRequesting {
    let projectID: String
    let customURL: URL?
    let customChainID: String?

    init(projectID: String, customURL: URL? = nil, customChainID: String? = nil) {
        self.projectID = projectID
        self.customURL = customURL
        self.customChainID = customChainID
    }

    func call(chainID: String, method: String, params: [Any]) async throws -> String {
        let url = try endpointURL(chainID: chainID)
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "jsonrpc": "2.0", "id": 1, "method": method, "params": params,
        ])
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode),
              let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw ReownRPCError.invalidResponse
        }
        if let error = json["error"] as? [String: Any] {
            throw ReownRPCError.remote(error["message"] as? String ?? "Chain RPC request failed")
        }
        guard let result = json["result"] as? String else { throw ReownRPCError.invalidResponse }
        return result
    }

    func endpointURL(chainID: String) throws -> URL {
        if let customURL {
            if let customChainID, customChainID != chainID {
                throw ReownRPCError.chainMismatch(expected: customChainID, actual: chainID)
            }
            return customURL
        }
        var components = URLComponents(string: "https://rpc.walletconnect.org/v1")!
        components.queryItems = [
            URLQueryItem(name: "chainId", value: "eip155:\(chainID)"),
            URLQueryItem(name: "projectId", value: projectID),
        ]
        guard let url = components.url else { throw ReownRPCError.invalidURL }
        return url
    }
}
