import Foundation

struct WeatherDayItem: Equatable {
    let date: Date
    let condition: String
    let high: String
    let low: String
    let precipitation: String
}

struct WeatherSnapshotContent: Equatable {
    let location: String
    let condition: String
    let temperature: String
    let apparentTemperature: String
    let humidity: String
    let wind: String
    let days: [WeatherDayItem]
}

enum WeatherSnapshotEncoder {
    static let maxDays = 5
    static let wireSize = 478
    static let validityInterval: TimeInterval = 3 * 60 * 60

    static func fingerprint(_ content: WeatherSnapshotContent) -> String {
        let days = content.days.prefix(maxDays).map {
            [String($0.date.timeIntervalSince1970), $0.condition, $0.high, $0.low, $0.precipitation]
                .joined(separator: "\u{1f}")
        }
        return ([content.location, content.condition, content.temperature, content.apparentTemperature,
                 content.humidity, content.wind] + days).joined(separator: "\u{1e}")
    }

    static func encode(_ content: WeatherSnapshotContent, sequence: UInt32, now: Date = Date()) -> Data {
        let dayFormatter = DateFormatter()
        dayFormatter.locale = .autoupdatingCurrent
        dayFormatter.setLocalizedDateFormatFromTemplate("EEE")

        let selected = Array(content.days.prefix(maxDays))
        var writer = WireWriter()
        writer.appendBytes([0x58, 0x33, 0x57, 0x54]) // X3WT
        writer.appendUInt8(1)
        writer.appendUInt8(UInt8(selected.count))
        writer.appendUInt16(UInt16(wireSize))
        writer.appendUInt32(sequence)
        writer.appendUInt32(UInt32(now.timeIntervalSince1970))
        writer.appendUInt32(UInt32(now.addingTimeInterval(validityInterval).timeIntervalSince1970))
        writer.appendFixedString(content.location, count: 40)
        writer.appendFixedString(content.condition, count: 32)
        writer.appendFixedString(content.temperature, count: 12)
        writer.appendFixedString(content.apparentTemperature, count: 12)
        writer.appendFixedString(content.humidity, count: 12)
        writer.appendFixedString(content.wind, count: 16)

        for index in 0..<maxDays {
            guard index < selected.count else {
                writer.appendZeroes(66)
                continue
            }
            let day = selected[index]
            writer.appendFixedString(dayFormatter.string(from: day.date), count: 12)
            writer.appendFixedString(day.condition, count: 24)
            writer.appendFixedString(day.high, count: 10)
            writer.appendFixedString(day.low, count: 10)
            writer.appendFixedString(day.precipitation, count: 10)
        }

        precondition(writer.data.count == wireSize - 4)
        writer.appendUInt32(CRC32.checksum(writer.data))
        precondition(writer.data.count == wireSize)
        return writer.data
    }
}
