#include "WeatherSnapshotStore.h"

#include <HalStorage.h>
#include <Logging.h>

namespace phone_sync {
namespace {

constexpr char SNAPSHOT_PATH[] = "/.crosspoint/phone-weather.bin";
constexpr char SNAPSHOT_TEMP_PATH[] = "/.crosspoint/phone-weather.tmp";
constexpr char SNAPSHOT_BACKUP_PATH[] = "/.crosspoint/phone-weather.bak";

}  // namespace

WeatherSnapshotStore& WeatherSnapshotStore::getInstance() {
  static WeatherSnapshotStore instance;
  return instance;
}

bool WeatherSnapshotStore::readSnapshot(const char* path, WeatherSnapshot& destination) const {
  HalFile file;
  if (!Storage.openFileForRead("WTS", path, file)) return false;
  const bool exactSize = file.fileSize() == sizeof(WeatherSnapshot);
  const size_t bytesRead = exactSize ? file.read(&destination, sizeof(destination)) : 0;
  return bytesRead == sizeof(destination) && validateWeatherSnapshot(destination);
}

bool WeatherSnapshotStore::load() {
  WeatherSnapshot candidate{};
  bool valid = readSnapshot(SNAPSHOT_PATH, candidate);
  if (!valid) valid = readSnapshot(SNAPSHOT_BACKUP_PATH, candidate);
  if (!valid) return false;

  std::lock_guard<std::mutex> lock(mutex);
  snapshot = candidate;
  loaded = true;
  return true;
}

bool WeatherSnapshotStore::writeSnapshot(const WeatherSnapshot& value) {
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.remove(SNAPSHOT_TEMP_PATH);

  HalFile file;
  if (!Storage.openFileForWrite("WTS", SNAPSHOT_TEMP_PATH, file)) return false;
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
    if (Storage.exists(SNAPSHOT_BACKUP_PATH)) Storage.rename(SNAPSHOT_BACKUP_PATH, SNAPSHOT_PATH);
    return false;
  }
  Storage.remove(SNAPSHOT_BACKUP_PATH);
  return true;
}

WeatherSnapshotStore::SaveResult WeatherSnapshotStore::save(const WeatherSnapshot& incoming) {
  if (!validateWeatherSnapshot(incoming)) return SaveResult::Error;

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (loaded && incoming.sequence < snapshot.sequence) return SaveResult::Stale;
    if (loaded && incoming.sequence == snapshot.sequence && incoming.crc32 == snapshot.crc32) {
      return SaveResult::Unchanged;
    }
  }

  if (!writeSnapshot(incoming)) {
    LOG_ERR("WTS", "Failed to persist phone weather snapshot");
    return SaveResult::Error;
  }

  std::lock_guard<std::mutex> lock(mutex);
  snapshot = incoming;
  loaded = true;
  return SaveResult::Updated;
}

bool WeatherSnapshotStore::copySnapshot(WeatherSnapshot& destination) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (!loaded) return false;
  destination = snapshot;
  return true;
}

}  // namespace phone_sync
