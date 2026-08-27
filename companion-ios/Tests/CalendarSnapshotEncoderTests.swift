import XCTest
@testable import X3Companion

final class CalendarSnapshotEncoderTests: XCTestCase {
    func testWireLayoutAndCRC() {
        let start = Date(timeIntervalSince1970: 1_777_000_000)
        let event = CalendarItem(identifier: "one", start: start, end: start.addingTimeInterval(1800),
                                 isAllDay: false, title: "Stand-up", location: "Room 1")
        let data = CalendarSnapshotEncoder.encode(day: start, events: [event], sequence: 42, now: start)

        XCTAssertEqual(data.count, 1_248)
        XCTAssertEqual(Array(data.prefix(4)), [0x58, 0x33, 0x43, 0x4c])
        XCTAssertEqual(data[4], 1)
        XCTAssertEqual(data[5], 1)

        let storedCRC = data.suffix(4).enumerated().reduce(UInt32(0)) { partial, pair in
            partial | (UInt32(pair.element) << UInt32(pair.offset * 8))
        }
        XCTAssertEqual(storedCRC, CRC32.checksum(data.prefix(1_244)))
    }

    func testUTF8TruncationDoesNotSplitScalar() {
        let start = Date(timeIntervalSince1970: 1_777_000_000)
        let event = CalendarItem(identifier: "two", start: start, end: start, isAllDay: true,
                                 title: String(repeating: "📅", count: 40), location: "")
        let data = CalendarSnapshotEncoder.encode(day: start, events: [event], sequence: 1, now: start)
        let titleStart = 60 + 44
        let titleBytes = data[titleStart..<(titleStart + 64)].prefix { $0 != 0 }
        XCTAssertNotNil(String(data: titleBytes, encoding: .utf8))
    }
}
