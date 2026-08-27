import SwiftUI

struct RPCSettingsView: View {
    @ObservedObject var settings: RPCSettings
    @State private var useCustom: Bool
    @State private var endpoint: String
    @State private var status: String?
    @State private var isTesting = false
    @FocusState private var endpointFocused: Bool

    init(settings: RPCSettings) {
        self.settings = settings
        _useCustom = State(initialValue: settings.customURL != nil)
        _endpoint = State(initialValue: settings.customURL?.absoluteString ?? "")
    }

    var body: some View {
        Form {
            Picker("RPC", selection: $useCustom) {
                Text("WalletConnect").tag(false)
                Text("Custom").tag(true)
            }
            .pickerStyle(.segmented)
            .onChange(of: useCustom) { _, custom in
                guard !custom else { return }
                do {
                    try settings.useWalletConnect()
                    status = "Using WalletConnect RPC"
                } catch {
                    status = error.localizedDescription
                }
            }

            if useCustom {
                Section("Endpoint") {
                    TextField("https://rpc.example.com", text: $endpoint)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .focused($endpointFocused)

                    Button {
                        endpointFocused = false
                        testAndSave()
                    } label: {
                        if isTesting {
                            HStack {
                                ProgressView()
                                Text("Testing")
                            }
                        } else {
                            Text("Test and Save")
                        }
                    }
                    .disabled(isTesting || endpoint.isEmpty)
                }
            }

            if let status {
                Text(status)
                    .foregroundStyle(status.hasPrefix("Connected") || status.hasPrefix("Using")
                                     ? WalletPalette.positive : WalletPalette.warning)
            }
        }
        .navigationTitle("RPC")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func testAndSave() {
        guard let url = RPCSettings.validatedURL(endpoint) else {
            status = RPCSettingsError.invalidURL.localizedDescription
            return
        }
        isTesting = true
        status = nil
        Task {
            do {
                let result = try await ReownRPCClient(projectID: "", customURL: url).call(
                    chainID: "1", method: "eth_chainId", params: []
                )
                guard result.hasPrefix("0x"), UInt64(result.dropFirst(2), radix: 16) != nil else {
                    throw ReownRPCError.invalidResponse
                }
                let chainID = UInt64(result.dropFirst(2), radix: 16)!
                try settings.useCustom(url, chainID: String(chainID))
                status = "Connected · Chain \(chainID)"
            } catch {
                status = error.localizedDescription
            }
            isTesting = false
        }
    }
}
