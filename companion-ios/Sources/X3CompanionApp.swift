import SwiftUI

@main
struct X3CompanionApp: App {
    @StateObject private var model = CompanionModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .onOpenURL { model.walletConnect.handle(url: $0) }
        }
    }
}
