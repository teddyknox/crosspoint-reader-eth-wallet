import SwiftUI
import UIKit

struct WalletHomeView: View {
    @EnvironmentObject private var model: CompanionModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var copied = false
    @State private var copyFeedback = 0

    private var signerReady: Bool {
        model.bluetoothReady && model.walletAddress.isUsableEvmAddress
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                accountCard
                actionsCard

                Label("Test funds only", systemImage: "flask")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .padding(16)
        }
        .background(WalletPalette.background)
        .navigationTitle("Wallet")
        .navigationBarTitleDisplayMode(.inline)
        .tint(WalletPalette.accent)
        .sensoryFeedback(.success, trigger: copyFeedback)
    }

    private var accountCard: some View {
        WalletCard {
            HStack(spacing: 12) {
                Image(systemName: "diamond.fill")
                    .font(.title2)
                    .foregroundStyle(WalletPalette.accent)
                    .frame(width: 42, height: 42)
                    .background(WalletPalette.accent.opacity(0.10), in: Circle())

                VStack(alignment: .leading, spacing: 5) {
                    Text("Ethereum")
                        .font(.headline)
                    Button {
                        guard model.walletAddress.isUsableEvmAddress else { return }
                        UIPasteboard.general.string = model.walletAddress
                        copyFeedback += 1
                        withAnimation(reduceMotion ? nil : .snappy) { copied = true }
                        Task {
                            try? await Task.sleep(for: .seconds(1.5))
                            withAnimation(reduceMotion ? nil : .snappy) { copied = false }
                        }
                    } label: {
                        HStack(spacing: 6) {
                            Text(model.walletAddress.isUsableEvmAddress
                                 ? model.walletAddress.shortenedEvmAddress
                                 : "Open Wallet on X3")
                                .font(.caption.monospaced())
                            if model.walletAddress.isUsableEvmAddress {
                                Image(systemName: copied ? "checkmark" : "doc.on.doc")
                                    .font(.caption2.weight(.bold))
                            }
                        }
                    }
                    .buttonStyle(.plain)
                    .disabled(!model.walletAddress.isUsableEvmAddress)
                }

                Spacer()
                Circle()
                    .fill(signerReady ? WalletPalette.positive : WalletPalette.warning)
                    .frame(width: 9, height: 9)
                    .accessibilityLabel(signerReady ? "X3 connected" : "X3 unavailable")
            }
        }
    }

    private var actionsCard: some View {
        WalletCard {
            VStack(spacing: 16) {
                NavigationLink {
                    EvmWalletView()
                } label: {
                    actionRow(icon: "arrow.up.right", title: "Send")
                }
                .buttonStyle(.plain)

                NavigationLink {
                    ReceiveView(address: model.walletAddress)
                } label: {
                    actionRow(icon: "arrow.down.left", title: "Receive")
                }
                .buttonStyle(.plain)
                .disabled(!model.walletAddress.isUsableEvmAddress)

                NavigationLink {
                    WalletConnectView(manager: model.walletConnect)
                } label: {
                    actionRow(icon: "viewfinder", title: "WalletConnect")
                }
                .buttonStyle(.plain)

                NavigationLink {
                    RPCSettingsView(settings: model.rpcSettings)
                } label: {
                    actionRow(icon: "server.rack", title: "RPC", detail: model.rpcSettings.label)
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func actionRow(icon: String, title: String, detail: String? = nil) -> some View {
        HStack(spacing: 13) {
            Image(systemName: icon)
                .font(.body.weight(.semibold))
                .foregroundStyle(WalletPalette.accent)
                .frame(width: 34, height: 34)
                .background(WalletPalette.accent.opacity(0.10), in: RoundedRectangle(cornerRadius: 10))
            Text(title)
                .font(.body.weight(.medium))
                .foregroundStyle(.primary)
            Spacer()
            if let detail {
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.tertiary)
        }
        .contentShape(Rectangle())
    }
}
