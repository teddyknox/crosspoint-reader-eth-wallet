import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: CompanionModel

    var body: some View {
        NavigationStack {
            Form {
                Section("Calendar") {
                    LabeledContent("Access", value: model.calendarAccessText)
                    LabeledContent("Today's events", value: "\(model.eventCount)")
                    if !model.hasCalendarAccess {
                        Button("Allow Calendar Access") {
                            Task { await model.requestCalendarAccess() }
                        }
                    }
                }

                Section("Xteink X3") {
                    LabeledContent("Bluetooth", value: model.bluetoothText)
                    LabeledContent("Sync", value: model.syncText)
                    Button("Sync Now") {
                        model.syncNow()
                    }
                    .disabled(!model.hasCalendarAccess || !model.bluetoothReady)
                }

                Section("Wallet") {
                    NavigationLink {
                        WalletHomeView()
                    } label: {
                        Label("Ethereum", systemImage: "wallet.bifold.fill")
                    }
                }

                Section {
                    Text("Keep Bluetooth enabled. The app keeps a service-filtered background scan active; when the X3 wakes and advertises, iOS can reconnect and send a fresh snapshot. iOS scheduling is best-effort, so missed windows retry on the next X3 wake.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("X3 Companion")
            .task { await model.start() }
        }
    }
}
