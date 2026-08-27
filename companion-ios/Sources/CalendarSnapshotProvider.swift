import EventKit
import Foundation

struct CalendarSnapshotResult {
    let data: Data
    let eventCount: Int
}

struct CalendarOption: Identifiable, Equatable {
    let id: String
    let title: String
    let sourceTitle: String
}

final class CalendarSelectionStore {
    private let defaults: UserDefaults
    private let selectionKey = "selectedCalendarIdentifiersV1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    // nil means "all calendars", including calendars added in the future.
    var selectedIdentifiers: Set<String>? {
        guard defaults.object(forKey: selectionKey) != nil else { return nil }
        return Set(defaults.stringArray(forKey: selectionKey) ?? [])
    }

    func setSelected(_ selected: Bool, identifier: String, availableIdentifiers: Set<String>) {
        var identifiers = selectedIdentifiers ?? availableIdentifiers
        if selected {
            identifiers.insert(identifier)
        } else {
            identifiers.remove(identifier)
        }
        defaults.set(Array(identifiers).sorted(), forKey: selectionKey)
    }

    func selectAll() {
        defaults.removeObject(forKey: selectionKey)
    }

    func selectNone() {
        defaults.set([String](), forKey: selectionKey)
    }
}

final class CalendarSnapshotProvider {
    private let eventStore = EKEventStore()
    private let cache = SnapshotCache()
    private let selection = CalendarSelectionStore()

    var hasFullAccess: Bool {
        EKEventStore.authorizationStatus(for: .event) == .fullAccess
    }

    func requestAccess() async throws -> Bool {
        try await eventStore.requestFullAccessToEvents()
    }

    var calendarOptions: [CalendarOption] {
        guard hasFullAccess else { return [] }
        return eventStore.calendars(for: .event)
            .map {
                CalendarOption(
                    id: $0.calendarIdentifier,
                    title: $0.title,
                    sourceTitle: $0.source.title
                )
            }
            .sorted {
                let sourceOrder = $0.sourceTitle.localizedCaseInsensitiveCompare($1.sourceTitle)
                if sourceOrder != .orderedSame { return sourceOrder == .orderedAscending }
                return $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending
            }
    }

    var selectedCalendarIdentifiers: Set<String>? {
        selection.selectedIdentifiers
    }

    func setCalendarSelected(_ selected: Bool, identifier: String) {
        selection.setSelected(
            selected,
            identifier: identifier,
            availableIdentifiers: Set(calendarOptions.map(\.id))
        )
    }

    func selectAllCalendars() {
        selection.selectAll()
    }

    func selectNoCalendars() {
        selection.selectNone()
    }

    func snapshot() async throws -> CalendarSnapshotResult {
        guard hasFullAccess else { throw CompanionError.calendarAccessRequired }

        let calendar = Calendar.autoupdatingCurrent
        let start = calendar.startOfDay(for: Date())
        guard let end = calendar.date(byAdding: .day, value: 1, to: start) else {
            throw CompanionError.unavailable
        }
        let selectedIdentifiers = selection.selectedIdentifiers
        let selectedCalendars = selectedIdentifiers.map { identifiers in
            eventStore.calendars(for: .event).filter { identifiers.contains($0.calendarIdentifier) }
        }
        let matchingEvents: [EKEvent]
        if selectedIdentifiers != nil, selectedCalendars?.isEmpty == true {
            matchingEvents = []
        } else {
            let predicate = eventStore.predicateForEvents(
                withStart: start,
                end: end,
                calendars: selectedCalendars
            )
            matchingEvents = eventStore.events(matching: predicate)
        }
        let events = matchingEvents
            .filter { $0.status != .canceled }
            .sorted { $0.startDate < $1.startDate }
            .prefix(CalendarSnapshotEncoder.maxEvents)
            .map {
                CalendarItem(
                    identifier: $0.eventIdentifier ?? "",
                    start: $0.startDate,
                    end: $0.endDate,
                    isAllDay: $0.isAllDay,
                    title: $0.title ?? "Untitled event",
                    location: $0.location ?? ""
                )
            }

        let items = Array(events)
        return CalendarSnapshotResult(data: cache.snapshot(for: start, events: items), eventCount: items.count)
    }
}

private final class SnapshotCache {
    private let defaults = UserDefaults.standard
    private let snapshotKey = "cachedCalendarSnapshotV1"
    private let fingerprintKey = "cachedCalendarFingerprintV1"
    private let sequenceKey = "calendarSnapshotSequenceV1"

    func snapshot(for day: Date, events: [CalendarItem]) -> Data {
        let fingerprint = CalendarSnapshotEncoder.fingerprint(day: day, events: events)
        if defaults.string(forKey: fingerprintKey) == fingerprint,
           let cached = defaults.data(forKey: snapshotKey),
           cached.count == CalendarSnapshotEncoder.wireSize {
            return cached
        }

        let previous = defaults.object(forKey: sequenceKey) as? NSNumber
        let initial = UInt32(Date().timeIntervalSince1970)
        let sequence = previous.map { $0.uint32Value &+ 1 } ?? initial
        let snapshot = CalendarSnapshotEncoder.encode(day: day, events: events, sequence: sequence)
        defaults.set(snapshot, forKey: snapshotKey)
        defaults.set(fingerprint, forKey: fingerprintKey)
        defaults.set(NSNumber(value: sequence), forKey: sequenceKey)
        return snapshot
    }
}
