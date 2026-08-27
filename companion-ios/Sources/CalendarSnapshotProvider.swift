import EventKit
import Foundation

struct CalendarSnapshotResult {
    let data: Data
    let eventCount: Int
}

final class CalendarSnapshotProvider {
    private let eventStore = EKEventStore()
    private let cache = SnapshotCache()

    var hasFullAccess: Bool {
        EKEventStore.authorizationStatus(for: .event) == .fullAccess
    }

    func requestAccess() async throws -> Bool {
        try await eventStore.requestFullAccessToEvents()
    }

    func snapshot() async throws -> CalendarSnapshotResult {
        guard hasFullAccess else { throw CompanionError.calendarAccessRequired }

        let calendar = Calendar.autoupdatingCurrent
        let start = calendar.startOfDay(for: Date())
        guard let end = calendar.date(byAdding: .day, value: 1, to: start) else {
            throw CompanionError.unavailable
        }
        let predicate = eventStore.predicateForEvents(withStart: start, end: end, calendars: nil)
        let events = eventStore.events(matching: predicate)
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
