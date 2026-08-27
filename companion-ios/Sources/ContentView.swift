import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: CompanionModel

    var body: some View {
        NavigationStack {
            Form {
                Section("Calendar") {
                    LabeledContent("Access", value: model.calendarAccessText)
                    LabeledContent("Today's events", value: "\(model.eventCount)")
                    if model.hasCalendarAccess {
                        NavigationLink {
                            CalendarSelectionView()
                        } label: {
                            LabeledContent("Shown calendars", value: model.calendarSelectionText)
                        }
                    }
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

private struct CalendarSelectionView: View {
    @EnvironmentObject private var model: CompanionModel

    var body: some View {
        List {
            Section {
                Button("Select All") {
                    model.selectAllCalendars()
                }
                Button("Select None") {
                    model.selectNoCalendars()
                }
            }

            ForEach(groupedCalendars, id: \.source) { group in
                Section(group.source) {
                    ForEach(group.calendars) { calendar in
                        Button {
                            model.setCalendarSelected(
                                !model.isCalendarSelected(calendar.id),
                                identifier: calendar.id
                            )
                        } label: {
                            HStack {
                                Text(calendar.title)
                                    .foregroundStyle(.primary)
                                Spacer()
                                if model.isCalendarSelected(calendar.id) {
                                    Image(systemName: "checkmark")
                                        .fontWeight(.semibold)
                                }
                            }
                            .contentShape(Rectangle())
                        }
                    }
                }
            }

            Section {
                Text("Your selection is used on the next X3 sync. “All calendars” also includes calendars you add later.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Calendars")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { model.refreshCalendars() }
    }

    private var groupedCalendars: [(source: String, calendars: [CalendarOption])] {
        Dictionary(grouping: model.calendarOptions, by: \.sourceTitle)
            .map { (source: $0.key, calendars: $0.value) }
            .sorted { $0.source.localizedCaseInsensitiveCompare($1.source) == .orderedAscending }
    }
}
