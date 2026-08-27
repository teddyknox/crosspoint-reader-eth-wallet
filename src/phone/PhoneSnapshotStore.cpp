#include "PhoneSnapshotStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace phone_sync {
namespace {

constexpr char SNAPSHOT_PATH[] = "/.crosspoint/phone-calendar.bin";
constexpr char SNAPSHOT_TEMP_PATH[] = "/.crosspoint/phone-calendar.tmp";
constexpr char SNAPSHOT_BACKUP_PATH[] = "/.crosspoint/phone-calendar.bak";

}  // namespace

PhoneSnapshotStore& PhoneSnapshotStore::getInstance() {
  static PhoneSnapshotStore instance;
  return instance;
}

bool PhoneSnapshotStore::readSnapshot(const char* path, CalendarSnapshot& destination) const {
  HalFile file;
  if (!Storage.openFileForRead("PHS", path, file)) return false;
  const bool exactSize = file.fileSize() == sizeof(CalendarSnapshot);
  const size_t bytesRead = exactSize ? file.read(&destination, sizeof(destination)) : 0;
  file.close();
  return bytesRead == sizeof(destination) && validateSnapshot(destination);
}

bool PhoneSnapshotStore::load() {
  CalendarSnapshot candidate{};
  bool valid = readSnapshot(SNAPSHOT_PATH, candidate);
  if (!valid) {
    valid = readSnapshot(SNAPSHOT_BACKUP_PATH, candidate);
  }
  if (!valid) return false;

  std::lock_guard<std::mutex> lock(mutex);
  snapshot = candidate;
  loaded = true;
  return true;
}

bool PhoneSnapshotStore::writeSnapshot(const CalendarSnapshot& value) {
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.remove(SNAPSHOT_TEMP_PATH);

  HalFile file;
  if (!Storage.openFileForWrite("PHS", SNAPSHOT_TEMP_PATH, file)) return false;
  const size_t bytesWritten = file.write(&value, sizeof(value));
  file.flush();
  file.close();
  if (bytesWritten != sizeof(value)) {
    Storage.remove(SNAPSHOT_TEMP_PATH);
    return false;
  }

  Storage.remove(SNAPSHOT_BACKUP_PATH);
  if (Storage.exists(SNAPSHOT_PATH) && !Storage.rename(SNAPSHOT_PATH, SNAPSHOT_BACKUP_PATH)) {
    Storage.remove(SNAPSHOT_TEMP_PATH);
    return false;
  }
  if (!Storage.rename(SNAPSHOT_TEMP_PATH, SNAPSHOT_PATH)) {
    if (Storage.exists(SNAPSHOT_BACKUP_PATH)) {
      Storage.rename(SNAPSHOT_BACKUP_PATH, SNAPSHOT_PATH);
    }
    return false;
  }
  Storage.remove(SNAPSHOT_BACKUP_PATH);
  return true;
}

PhoneSnapshotStore::SaveResult PhoneSnapshotStore::save(const CalendarSnapshot& incoming) {
  if (!validateSnapshot(incoming)) return SaveResult::Error;

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (loaded && incoming.sequence < snapshot.sequence) return SaveResult::Stale;
    if (loaded && incoming.sequence == snapshot.sequence && incoming.crc32 == snapshot.crc32) {
      return SaveResult::Unchanged;
    }
  }

  // Do not hold the snapshot mutex across SD I/O: the render task may still be
  // painting the prior snapshot while this durable replacement is written.
  if (!writeSnapshot(incoming)) {
    LOG_ERR("PHS", "Failed to persist phone calendar snapshot");
    return SaveResult::Error;
  }

  std::lock_guard<std::mutex> lock(mutex);
  snapshot = incoming;
  loaded = true;
  return SaveResult::Updated;
}

bool PhoneSnapshotStore::copySnapshot(CalendarSnapshot& destination) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (!loaded) return false;
  destination = snapshot;
  return true;
}

}  // namespace phone_sync
