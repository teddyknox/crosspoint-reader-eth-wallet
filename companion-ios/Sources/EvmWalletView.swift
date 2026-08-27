import SwiftUI
import UIKit

struct EvmWalletView: View {
    @EnvironmentObject private var model: CompanionModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var chainID = "11155111"
    @State private var nonce = "0"
    @State private var recipient = "0x000000000000000000000000000000000000dEaD"
    @State private var amount = "0.0001"
    @State private var gasLimit = "21000"
    @State private var priorityFee = "1"
    @State private var maxFee = "30"
    @State private var data = "0x"
    @State private var advancedExpanded = false
    @State private var automaticNetworkValues = true
    @State private var submitFeedback = 0
    @State private var copyFeedback = 0
    @FocusState private var focusedField: Field?

    private enum Field: Hashable {
        case recipient
        case amount
        case chainID
        case nonce
        case gasLimit
        case priorityFee
        case maxFee
        case data
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                transferCard
                advancedCard

                Button {
                    focusedField = nil
                    submitFeedback += 1
                    Task {
                        if let automatic = await model.signEvm(
                            chainID: chainID,
                            nonce: nonce,
                            recipient: recipient,
                            amount: amount,
                            gasLimit: gasLimit,
                            priorityFee: priorityFee,
                            maxFee: maxFee,
                            data: data,
                            automaticNetworkValues: automaticNetworkValues
                        ) {
                            nonce = automatic.nonce
                            gasLimit = automatic.gasLimit
                            priorityFee = automatic.priorityFeeGwei
                            maxFee = automatic.maxFeeGwei
                        }
                    }
                } label: {
                    HStack(spacing: 9) {
                        if model.isPreparingWalletTransaction {
                            ProgressView()
                                .tint(.white)
                        } else {
                            Image(systemName: "arrow.right.circle.fill")
                        }
                        Text(buttonTitle)
                    }
                }
                .buttonStyle(WalletPrimaryButtonStyle())
                .disabled(!model.bluetoothReady || model.isPreparingWalletTransaction || model.isWalletRequestPending)
                .sensoryFeedback(.impact(weight: .medium), trigger: submitFeedback)

                if let error = model.walletOperationError {
                    WalletCard {
                        Label("Transaction not ready", systemImage: "exclamationmark.triangle.fill")
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(WalletPalette.warning)
                        Text(error)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    .transition(.opacity.combined(with: .scale(scale: 0.98)))
                }

                if let signedTransaction = model.signedTransaction {
                    signedTransactionCard(signedTransaction)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }

            }
            .padding(16)
        }
        .background(WalletPalette.background)
        .navigationTitle("Send")
        .navigationBarTitleDisplayMode(.inline)
        .tint(WalletPalette.accent)
        .animation(reduceMotion ? nil : .snappy, value: model.signedTransaction != nil)
        .animation(reduceMotion ? nil : .snappy, value: model.walletOperationError)
        .sensoryFeedback(.success, trigger: copyFeedback)
        .toolbar {
            ToolbarItemGroup(placement: .keyboard) {
                Spacer()
                Button("Done") { focusedField = nil }
            }
        }
    }

    private var buttonTitle: String {
        if model.isPreparingWalletTransaction { return "Preparing transaction" }
        if model.isWalletRequestPending { return "Waiting for X3" }
        return "Review on X3"
    }

    private var transferCard: some View {
        WalletCard {
            VStack(alignment: .leading, spacing: 8) {
                Text("Recipient")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                TextField("0x…", text: $recipient)
                    .font(.system(.subheadline, design: .monospaced))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focusedField, equals: .recipient)
                    .padding(13)
                    .background(WalletPalette.background, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            }

            VStack(alignment: .leading, spacing: 8) {
                Text("Amount")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    TextField("0.00", text: $amount)
                        .font(.title2.weight(.semibold))
                        .keyboardType(.decimalPad)
                        .focused($focusedField, equals: .amount)
                    Text("ETH")
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(.secondary)
                }
                .padding(13)
                .background(WalletPalette.background, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            }
            .padding(.top, 14)
        }
    }

    private var advancedCard: some View {
        WalletCard {
            DisclosureGroup(isExpanded: $advancedExpanded) {
                VStack(spacing: 14) {
                    Toggle(isOn: $automaticNetworkValues) {
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Automatic network values")
                                .font(.subheadline.weight(.semibold))
                            Text("Refresh pending nonce, gas, and fees before review")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .tint(WalletPalette.accent)

                    HStack(spacing: 12) {
                        inputField("Chain ID", text: $chainID, field: .chainID, keyboard: .numberPad)
                        inputField("Nonce", text: $nonce, field: .nonce, keyboard: .numberPad,
                                   enabled: !automaticNetworkValues)
                    }
                    inputField("Gas limit", text: $gasLimit, field: .gasLimit, keyboard: .numberPad,
                               enabled: !automaticNetworkValues)
                    HStack(spacing: 12) {
                        inputField("Priority fee", text: $priorityFee, field: .priorityFee, keyboard: .decimalPad,
                                   suffix: "gwei", enabled: !automaticNetworkValues)
                        inputField("Maximum fee", text: $maxFee, field: .maxFee, keyboard: .decimalPad,
                                   suffix: "gwei", enabled: !automaticNetworkValues)
                    }
                    inputField("Data", text: $data, field: .data, monospaced: true)
                }
                .padding(.top, 16)
            } label: {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Transaction details")
                        .font(.headline)
                    Text(automaticNetworkValues ? "Automatic" : "Manual")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private func signedTransactionCard(_ signedTransaction: String) -> some View {
        WalletCard {
            HStack {
                WalletSectionTitle("Signed transaction")
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(WalletPalette.positive)
            }
            Text(signedTransaction)
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(4)
                .textSelection(.enabled)
                .padding(.vertical, 12)

            HStack(spacing: 10) {
                Button {
                    UIPasteboard.general.string = signedTransaction
                    copyFeedback += 1
                } label: {
                    Label("Copy", systemImage: "doc.on.doc")
                }
                .buttonStyle(WalletSecondaryButtonStyle())

                ShareLink(item: signedTransaction) {
                    Label("Share", systemImage: "square.and.arrow.up")
                }
                .buttonStyle(WalletSecondaryButtonStyle())
            }
        }
    }

    private func inputField(
        _ title: String,
        text: Binding<String>,
        field: Field,
        keyboard: UIKeyboardType = .default,
        monospaced: Bool = false,
        suffix: String? = nil,
        enabled: Bool = true
    ) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            HStack(spacing: 5) {
                TextField(title, text: text)
                    .font(monospaced ? .system(.subheadline, design: .monospaced) : .subheadline)
                    .keyboardType(keyboard)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focusedField, equals: field)
                if let suffix {
                    Text(suffix)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(12)
            .background(WalletPalette.background, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.58)
        }
        .frame(maxWidth: .infinity)
    }
}
