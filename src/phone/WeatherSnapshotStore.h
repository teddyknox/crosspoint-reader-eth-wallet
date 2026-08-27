#pragma once

#include <PhoneSyncProtocol.h>

#include <mutex>

namespace phone_sync {

class WeatherSnapshotStore {
 public:
  enum class SaveResult { Updated, Unchanged, Stale, Error };

  static WeatherSnapshotStore& getInstance();

  bool load();
  SaveResult save(const WeatherSnapshot& incoming);
  bool copySnapshot(WeatherSnapshot& destination) const;

 private:
  WeatherSnapshotStore() = default;

  bool readSnapshot(const char* path, WeatherSnapshot& destination) const;
  bool writeSnapshot(const WeatherSnapshot& value);

  mutable std::mutex mutex;
  WeatherSnapshot snapshot{};
  bool loaded = false;
};

}  // namespace phone_sync

#define WEATHER_SNAPSHOT_STORE phone_sync::WeatherSnapshotStore::getInstance()
