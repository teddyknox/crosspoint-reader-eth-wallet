import CoreImage.CIFilterBuiltins
import SwiftUI
import UIKit

struct ReceiveView: View {
    private static let qrContext = CIContext()

    let address: String
    @State private var copyFeedback = 0

    private var qrImage: UIImage? { Self.qrImage(for: address) }

    static func qrImage(for address: String) -> UIImage? {
        guard let payload = qrPayload(for: address) else { return nil }
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(payload.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage else { return nil }
        let scaled = output.transformed(by: CGAffineTransform(scaleX: 9, y: 9))
        guard let bitmap = qrContext.createCGImage(scaled, from: scaled.extent) else { return nil }
        return UIImage(cgImage: bitmap)
    }

    var body: some View {
        VStack(spacing: 22) {
            Spacer()

            if let qrImage {
                Image(uiImage: qrImage)
                    .interpolation(.none)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 260)
                    .padding(20)
                    .background(.white, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
            } else {
                ContentUnavailableView("Address unavailable", systemImage: "qrcode")
            }

            Text(address.isUsableEvmAddress ? address : "Open Wallet on X3")
                .font(.footnote.monospaced())
                .multilineTextAlignment(.center)
                .textSelection(.enabled)
                .padding(.horizontal)

            Button {
                guard address.isUsableEvmAddress else { return }
                UIPasteboard.general.string = address
                copyFeedback += 1
            } label: {
                Label("Copy address", systemImage: "doc.on.doc")
            }
            .buttonStyle(WalletPrimaryButtonStyle())
            .disabled(!address.isUsableEvmAddress)

            Spacer()
        }
        .padding(20)
        .background(WalletPalette.background)
        .navigationTitle("Receive")
        .navigationBarTitleDisplayMode(.inline)
        .sensoryFeedback(.success, trigger: copyFeedback)
    }

    static func qrPayload(for address: String) -> String? {
        address.isUsableEvmAddress ? "ethereum:\(address)" : nil
    }
}
