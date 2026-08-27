import XCTest
@testable import X3Companion

final class WeatherSnapshotEncoderTests: XCTestCase {
    func testWireLayoutAndCRC() {
        let now = Date(timeIntervalSince1970: 1_777_000_000)
        let content = WeatherSnapshotContent(
            location: "San Francisco",
            condition: "Mostly Clear",
            temperature: "62°F",
            apparentTemperature: "61°F",
            humidity: "72%",
            wind: "W 8 mph",
            days: [
                WeatherDayItem(date: now, condition: "Mostly Clear", high: "H 68°", low: "L 54°",
                               precipitation: "10%")
            ]
        )

        let data = WeatherSnapshotEncoder.encode(content, sequence: 7, now: now)

        XCTAssertEqual(data.count, WeatherSnapshotEncoder.wireSize)
        XCTAssertEqual(Array(data.prefix(4)), [0x58, 0x33, 0x57, 0x54])
        XCTAssertEqual(data[4], 1)
        XCTAssertEqual(data[5], 1)
        XCTAssertEqual(UInt16(data[6]) | UInt16(data[7]) << 8, UInt16(WeatherSnapshotEncoder.wireSize))
        let validUntil = UInt32(data[16]) | UInt32(data[17]) << 8 |
            UInt32(data[18]) << 16 | UInt32(data[19]) << 24
        XCTAssertEqual(validUntil, UInt32(now.addingTimeInterval(WeatherSnapshotEncoder.validityInterval).timeIntervalSince1970))
        let expectedCRC = CRC32.checksum(Data(data.dropLast(4)))
        let crcOffset = data.count - 4
        let actualCRC = UInt32(data[crcOffset]) | UInt32(data[crcOffset + 1]) << 8 |
            UInt32(data[crcOffset + 2]) << 16 | UInt32(data[crcOffset + 3]) << 24
        XCTAssertEqual(actualCRC, expectedCRC)
    }

    func testEncoderTruncatesDaysAndStringsOnUtf8Boundaries() {
        let day = WeatherDayItem(date: Date(), condition: String(repeating: "☀️", count: 30), high: "H 100°",
                                 low: "L 80°", precipitation: "100%")
        let content = WeatherSnapshotContent(
            location: String(repeating: "é", count: 30), condition: day.condition, temperature: "100°F",
            apparentTemperature: "105°F", humidity: "99%", wind: "WNW 120 mph",
            days: Array(repeating: day, count: 8)
        )

        let data = WeatherSnapshotEncoder.encode(content, sequence: 1)
        XCTAssertEqual(data[5], UInt8(WeatherSnapshotEncoder.maxDays))
        let location = data[20..<60].prefix { $0 != 0 }
        XCTAssertNotNil(String(data: location, encoding: .utf8))
    }
}
