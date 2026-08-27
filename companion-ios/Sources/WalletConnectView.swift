import SwiftUI
import UIKit

struct WalletConnectView: View {
    @ObservedObject var manager: WalletConnectManager
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var showingScanner = false
    @State private var scanFeedback = 0

    private var relayReady: Bool {
        !manager.projectID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var statusReady: Bool {
        manager.sessionCount > 0 || manager.status.localizedCaseInsensitiveContains("ready") ||
            manager.status.localizedCaseInsensitiveContains("active")
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                connectCard

                if let name = manager.proposalName {
                    proposalCard(name: name)
                        .transition(.move(edge: .top).combined(with: .opacity))
                }

                if let request = manager.requestSummary {
                    WalletCard {
                        HStack(alignment: .top, spacing: 13) {
                            Image(systemName: "signature")
                                .font(.title3)
                                .foregroundStyle(WalletPalette.warning)
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Approval requested")
                                    .font(.subheadline.weight(.semibold))
                                Text(request)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                                Text("Review the full request on your X3.")
                                    .font(.caption)
                                    .foregroundStyle(WalletPalette.warning)
                            }
                        }
                    }
                    .transition(.move(edge: .top).combined(with: .opacity))
                }

            }
            .padding(16)
        }
        .background(WalletPalette.background)
        .navigationTitle("WalletConnect")
        .navigationBarTitleDisplayMode(.inline)
        .tint(WalletPalette.accent)
        .animation(reduceMotion ? nil : .snappy, value: manager.proposalName)
        .animation(reduceMotion ? nil : .snappy, value: manager.requestSummary)
        .sensoryFeedback(.impact(weight: .light), trigger: scanFeedback)
        .sheet(isPresented: $showingScanner) {
            WalletConnectQRScannerSheet { manager.pair(scannedPayload: $0) }
        }
    }

    private var connectCard: some View {
        WalletCard {
            HStack(spacing: 10) {
                Circle()
                    .fill(statusReady ? WalletPalette.positive : WalletPalette.warning)
                    .frame(width: 9, height: 9)
                Text(manager.status)
                    .font(.subheadline.weight(.medium))
                    .lineLimit(2)
                Spacer()
                if manager.sessionCount > 0 {
                    Text("\(manager.sessionCount) connected")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Button {
                scanFeedback += 1
                showingScanner = true
            } label: {
                Label("Scan QR Code", systemImage: "camera.fill")
            }
            .buttonStyle(WalletPrimaryButtonStyle())
            .disabled(!relayReady)
            .padding(.top, 18)

            Button {
                guard let payload = UIPasteboard.general.string else { return }
                _ = manager.pair(scannedPayload: payload)
            } label: {
                Text("Paste connection link")
            }
            .buttonStyle(WalletSecondaryButtonStyle())
            .disabled(!relayReady)
            .padding(.top, 10)
        }
    }

    private func proposalCard(name: String) -> some View {
        WalletCard {
            WalletSectionTitle("Connection request", detail: "Review")
            HStack(spacing: 14) {
                Image(systemName: "globe")
                    .font(.title2)
                    .foregroundStyle(WalletPalette.accent)
                    .frame(width: 48, height: 48)
                    .background(WalletPalette.accent.opacity(0.10), in: RoundedRectangle(cornerRadius: 15))
                VStack(alignment: .leading, spacing: 3) {
                    Text(name)
                        .font(.headline)
                    Text(manager.proposalURL)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .padding(.vertical, 14)

            VStack(alignment: .leading, spacing: 5) {
                Text("Requested permissions")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                Text(manager.proposalMethods.joined(separator: " · "))
                    .font(.caption.monospaced())
                    .fixedSize(horizontal: false, vertical: true)
            }

            Button("Approve connection") { manager.approveProposal() }
                .buttonStyle(WalletPrimaryButtonStyle())
                .padding(.top, 16)
            Button("Reject", role: .destructive) { manager.rejectProposal() }
                .font(.subheadline.weight(.semibold))
                .frame(maxWidth: .infinity)
                .padding(.top, 4)
        }
    }

}
