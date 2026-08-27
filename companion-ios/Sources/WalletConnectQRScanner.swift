import SwiftUI
import Vision
import VisionKit

struct WalletConnectQRScannerSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var message = "Point the camera at a WalletConnect QR code"

    let onPayload: (String) -> Bool

    var body: some View {
        NavigationStack {
            ZStack(alignment: .bottom) {
                if DataScannerViewController.isSupported, DataScannerViewController.isAvailable {
                    WalletConnectQRScanner { payload in
                        if onPayload(payload) {
                            dismiss()
                        } else {
                            message = "That is not a WalletConnect QR code. Try another code."
                        }
                    } onFailure: { error in
                        message = error.localizedDescription
                    }
                    .ignoresSafeArea(edges: .bottom)
                } else {
                    ContentUnavailableView(
                        "Camera Scanner Unavailable",
                        systemImage: "qrcode.viewfinder",
                        description: Text("Allow camera access in Settings, or paste the pairing URI instead.")
                    )
                }

                Text(message)
                    .font(.callout.weight(.medium))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 18)
                    .padding(.vertical, 12)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
                    .padding()
            }
            .navigationTitle("Scan WalletConnect")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}

private struct WalletConnectQRScanner: UIViewControllerRepresentable {
    let onPayload: (String) -> Void
    let onFailure: (Error) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onPayload: onPayload, onFailure: onFailure)
    }

    func makeUIViewController(context: Context) -> DataScannerViewController {
        let scanner = DataScannerViewController(
            recognizedDataTypes: [.barcode(symbologies: [.qr])],
            qualityLevel: .balanced,
            recognizesMultipleItems: false,
            isHighFrameRateTrackingEnabled: true,
            isPinchToZoomEnabled: true,
            isGuidanceEnabled: true,
            isHighlightingEnabled: true
        )
        scanner.delegate = context.coordinator
        do {
            try scanner.startScanning()
        } catch {
            DispatchQueue.main.async { onFailure(error) }
        }
        return scanner
    }

    func updateUIViewController(_ scanner: DataScannerViewController, context _: Context) {
        guard !scanner.isScanning else { return }
        do {
            try scanner.startScanning()
        } catch {
            DispatchQueue.main.async { onFailure(error) }
        }
    }

    static func dismantleUIViewController(_ scanner: DataScannerViewController, coordinator _: Coordinator) {
        scanner.stopScanning()
    }

    @MainActor
    final class Coordinator: NSObject, DataScannerViewControllerDelegate {
        private let onPayload: (String) -> Void
        private let onFailure: (Error) -> Void
        private var lastPayload: String?

        init(onPayload: @escaping (String) -> Void, onFailure: @escaping (Error) -> Void) {
            self.onPayload = onPayload
            self.onFailure = onFailure
        }

        func dataScanner(_: DataScannerViewController, didAdd addedItems: [RecognizedItem],
                         allItems _: [RecognizedItem]) {
            acceptFirstBarcode(in: addedItems)
        }

        func dataScanner(_: DataScannerViewController, didUpdate updatedItems: [RecognizedItem],
                         allItems _: [RecognizedItem]) {
            acceptFirstBarcode(in: updatedItems)
        }

        func dataScanner(_: DataScannerViewController,
                         becameUnavailableWithError error: DataScannerViewController.ScanningUnavailable) {
            onFailure(error)
        }

        private func acceptFirstBarcode(in items: [RecognizedItem]) {
            for case .barcode(let barcode) in items {
                guard let payload = barcode.payloadStringValue, payload != lastPayload else { continue }
                lastPayload = payload
                onPayload(payload)
                return
            }
        }
    }
}
