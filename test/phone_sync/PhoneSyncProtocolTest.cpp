#include <gtest/gtest.h>

#include <cstring>

#include "PhoneSyncProtocol.h"

namespace {

phone_sync::CalendarSnapshot validSnapshot() {
  phone_sync::CalendarSnapshot snapshot{};
  std::memcpy(snapshot.magic, phone_sync::SNAPSHOT_MAGIC, sizeof(snapshot.magic));
  snapshot.version = phone_sync::PROTOCOL_VERSION;
  snapshot.eventCount = 1;
  snapshot.wireSize = sizeof(snapshot);
  snapshot.sequence = 42;
  std::strcpy(snapshot.dateLabel, "Wednesday, August 26");
  std::strcpy(snapshot.events[0].startLabel, "9:00 AM");
  std::strcpy(snapshot.events[0].endLabel, "9:30 AM");
  std::strcpy(snapshot.events[0].title, "Stand-up");
  snapshot.crc32 = phone_sync::snapshotCrc32(snapshot);
  return snapshot;
}

TEST(PhoneSyncProtocol, ValidSnapshotPasses) { EXPECT_TRUE(phone_sync::validateSnapshot(validSnapshot())); }

TEST(PhoneSyncProtocol, PayloadMutationFailsCrc) {
  auto snapshot = validSnapshot();
  snapshot.events[0].title[0] = 'B';
  EXPECT_FALSE(phone_sync::validateSnapshot(snapshot));
}

TEST(PhoneSyncProtocol, RejectsUnterminatedWireStrings) {
  auto snapshot = validSnapshot();
  std::memset(snapshot.events[0].title, 'x', sizeof(snapshot.events[0].title));
  snapshot.crc32 = phone_sync::snapshotCrc32(snapshot);
  EXPECT_FALSE(phone_sync::validateSnapshot(snapshot));
}

TEST(PhoneSyncProtocol, RejectsExcessEventCount) {
  auto snapshot = validSnapshot();
  snapshot.eventCount = phone_sync::MAX_EVENTS + 1;
  snapshot.crc32 = phone_sync::snapshotCrc32(snapshot);
  EXPECT_FALSE(phone_sync::validateSnapshot(snapshot));
}

}  // namespace
