import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: CompanionModel
    @Environment(\.colorScheme) private var colorScheme

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

                Section("Weather") {
                    LabeledContent("Location", value: model.weatherLocationText)
                    LabeledContent("Forecast", value: model.weatherSummaryText)
                    LabeledContent("Status") {
                        Text(model.weatherStatusText)
                            .multilineTextAlignment(.trailing)
                            .lineLimit(2)
                    }
                    if model.hasWeatherLocationAccess {
                        Button {
                            Task { await model.refreshWeather() }
                        } label: {
                            if model.isRefreshingWeather {
                                HStack {
                                    ProgressView()
                                    Text("Refreshing Weather")
                                }
                            } else {
                                Text("Refresh Weather")
                            }
                        }
                        .disabled(model.isRefreshingWeather)
                    } else {
                        Button("Enable Current-Location Weather") {
                            Task { await model.requestWeatherAccess() }
                        }
                        .disabled(model.isRefreshingWeather)
                    }

                    if let legalURL = model.weatherAttributionURL {
                        Link(destination: legalURL) {
                            HStack {
                                Group {
                                    if let weatherMarkURL {
                                        AsyncImage(url: weatherMarkURL) { phase in
                                            if let image = phase.image {
                                                image.resizable().scaledToFit()
                                            } else {
                                                Text("Apple Weather Attribution")
                                            }
                                        }
                                    } else {
                                        Text("Apple Weather Attribution")
                                    }
                                }
                                .frame(maxWidth: 180, maxHeight: 24, alignment: .leading)
                                Spacer()
                                Image(systemName: "arrow.up.right")
                                    .font(.footnote)
                            }
                        }
                    }
                }

                Section("Xteink X3") {
                    LabeledContent("Bluetooth", value: model.bluetoothText)
                    LabeledContent("Sync", value: model.syncText)
                    Button("Sync Now") {
                        model.syncNow()
                    }
                    .disabled((!model.hasCalendarAccess && !model.hasWeatherSnapshot) || !model.bluetoothReady)
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

    private var weatherMarkURL: URL? {
        colorScheme == .dark ? model.weatherAttributionMarkDarkURL : model.weatherAttributionMarkLightURL
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
