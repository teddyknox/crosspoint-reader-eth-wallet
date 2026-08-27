#pragma once

#include <PhoneSyncProtocol.h>

#include <mutex>

namespace phone_sync {

class PhoneSnapshotStore {
 public:
  enum class SaveResult { Updated, Unchanged, Stale, Error };

  static PhoneSnapshotStore& getInstance();

  bool load();
  SaveResult save(const CalendarSnapshot& incoming);
  bool copySnapshot(CalendarSnapshot& destination) const;

 private:
  PhoneSnapshotStore() = default;

  bool readSnapshot(const char* path, CalendarSnapshot& destination) const;
  bool writeSnapshot(const CalendarSnapshot& snapshot);

  mutable std::mutex mutex;
  CalendarSnapshot snapshot{};
  bool loaded = false;
};

}  // namespace phone_sync

#define PHONE_SNAPSHOT_STORE phone_sync::PhoneSnapshotStore::getInstance()
