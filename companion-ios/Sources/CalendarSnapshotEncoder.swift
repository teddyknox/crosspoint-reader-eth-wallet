import Foundation

struct CalendarItem {
    let identifier: String
    let start: Date
    let end: Date
    let isAllDay: Bool
    let title: String
    let location: String
}

enum CalendarSnapshotEncoder {
    static let maxEvents = 8
    static let wireSize = 1_248

    static func fingerprint(day: Date, events: [CalendarItem]) -> String {
        let parts = events.map {
            [$0.identifier, String($0.start.timeIntervalSince1970), String($0.end.timeIntervalSince1970),
             $0.isAllDay ? "1" : "0", $0.title, $0.location].joined(separator: "\u{1f}")
        }
        return ([String(Calendar.autoupdatingCurrent.startOfDay(for: day).timeIntervalSince1970)] + parts)
            .joined(separator: "\u{1e}")
    }

    static func encode(day: Date, events: [CalendarItem], sequence: UInt32, now: Date = Date()) -> Data {
        let calendar = Calendar.autoupdatingCurrent
        let dateFormatter = DateFormatter()
        dateFormatter.locale = .autoupdatingCurrent
        dateFormatter.calendar = calendar
        dateFormatter.setLocalizedDateFormatFromTemplate("EEEE MMMM d")

        let timeFormatter = DateFormatter()
        timeFormatter.locale = .autoupdatingCurrent
        timeFormatter.calendar = calendar
        timeFormatter.timeStyle = .short
        timeFormatter.dateStyle = .none

        let selected = Array(events.prefix(maxEvents))
        var writer = WireWriter()
        writer.appendBytes([0x58, 0x33, 0x43, 0x4c]) // X3CL
        writer.appendUInt8(1)
        writer.appendUInt8(UInt8(selected.count))
        writer.appendUInt16(UInt16(wireSize))
        writer.appendUInt32(sequence)
        writer.appendUInt32(UInt32(now.timeIntervalSince1970))
        let validUntil = calendar.date(byAdding: .day, value: 1, to: calendar.startOfDay(for: day)) ?? now
        writer.appendUInt32(UInt32(validUntil.timeIntervalSince1970))
        writer.appendFixedString(dateFormatter.string(from: day), count: 40)

        for index in 0..<maxEvents {
            guard index < selected.count else {
                writer.appendZeroes(148)
                continue
            }
            let event = selected[index]
            writer.appendUInt8(event.isAllDay ? 1 : 0)
            writer.appendZeroes(3)
            writer.appendUInt32(UInt32(event.start.timeIntervalSince1970))
            writer.appendUInt32(UInt32(event.end.timeIntervalSince1970))
            writer.appendFixedString(event.isAllDay ? "" : timeFormatter.string(from: event.start), count: 16)
            writer.appendFixedString(event.isAllDay ? "" : timeFormatter.string(from: event.end), count: 16)
            writer.appendFixedString(event.title, count: 64)
            writer.appendFixedString(event.location, count: 40)
        }

        precondition(writer.data.count == wireSize - 4)
        writer.appendUInt32(CRC32.checksum(writer.data))
        precondition(writer.data.count == wireSize)
        return writer.data
    }
}

private struct WireWriter {
    var data = Data()

    mutating func appendUInt8(_ value: UInt8) { data.append(value) }

    mutating func appendUInt16(_ value: UInt16) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
    }

    mutating func appendUInt32(_ value: UInt32) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }

    mutating func appendBytes(_ bytes: [UInt8]) { data.append(contentsOf: bytes) }

    mutating func appendZeroes(_ count: Int) { data.append(contentsOf: repeatElement(0, count: count)) }

    mutating func appendFixedString(_ value: String, count: Int) {
        var encoded = Data(value.utf8.prefix(count - 1))
        while String(data: encoded, encoding: .utf8) == nil && !encoded.isEmpty {
            encoded.removeLast()
        }
        data.append(encoded)
        appendZeroes(count - encoded.count)
    }
}

enum CRC32 {
    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xffff_ffff
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 {
                let mask = UInt32(bitPattern: -Int32(crc & 1))
                crc = (crc >> 1) ^ (0xedb8_8320 & mask)
            }
        }
        return ~crc
    }
}
