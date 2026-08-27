#include "PhoneSyncProtocol.h"

#include <cstring>

namespace phone_sync {
namespace {

bool containsNull(const char* value, const size_t capacity) { return std::memchr(value, '\0', capacity) != nullptr; }

}  // namespace

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

uint32_t snapshotCrc32(const CalendarSnapshot& snapshot) {
  return crc32(reinterpret_cast<const uint8_t*>(&snapshot), offsetof(CalendarSnapshot, crc32));
}

bool validateSnapshot(const CalendarSnapshot& snapshot) {
  if (std::memcmp(snapshot.magic, SNAPSHOT_MAGIC, sizeof(SNAPSHOT_MAGIC)) != 0 ||
      snapshot.version != PROTOCOL_VERSION || snapshot.wireSize != sizeof(CalendarSnapshot) ||
      snapshot.eventCount > MAX_EVENTS || !containsNull(snapshot.dateLabel, sizeof(snapshot.dateLabel)) ||
      snapshot.crc32 != snapshotCrc32(snapshot)) {
    return false;
  }

  for (size_t i = 0; i < snapshot.eventCount; ++i) {
    const auto& event = snapshot.events[i];
    if (!containsNull(event.startLabel, sizeof(event.startLabel)) ||
        !containsNull(event.endLabel, sizeof(event.endLabel)) || !containsNull(event.title, sizeof(event.title)) ||
        !containsNull(event.location, sizeof(event.location))) {
      return false;
    }
  }
  return true;
}

uint32_t weatherSnapshotCrc32(const WeatherSnapshot& snapshot) {
  return crc32(reinterpret_cast<const uint8_t*>(&snapshot), offsetof(WeatherSnapshot, crc32));
}

bool validateWeatherSnapshot(const WeatherSnapshot& snapshot) {
  if (std::memcmp(snapshot.magic, WEATHER_SNAPSHOT_MAGIC, sizeof(WEATHER_SNAPSHOT_MAGIC)) != 0 ||
      snapshot.version != WEATHER_PROTOCOL_VERSION || snapshot.wireSize != sizeof(WeatherSnapshot) ||
      snapshot.dayCount > MAX_FORECAST_DAYS || !containsNull(snapshot.location, sizeof(snapshot.location)) ||
      !containsNull(snapshot.condition, sizeof(snapshot.condition)) ||
      !containsNull(snapshot.temperature, sizeof(snapshot.temperature)) ||
      !containsNull(snapshot.apparentTemperature, sizeof(snapshot.apparentTemperature)) ||
      !containsNull(snapshot.humidity, sizeof(snapshot.humidity)) ||
      !containsNull(snapshot.wind, sizeof(snapshot.wind)) || snapshot.crc32 != weatherSnapshotCrc32(snapshot)) {
    return false;
  }

  for (size_t i = 0; i < snapshot.dayCount; ++i) {
    const auto& day = snapshot.days[i];
    if (!containsNull(day.dayLabel, sizeof(day.dayLabel)) || !containsNull(day.condition, sizeof(day.condition)) ||
        !containsNull(day.highLabel, sizeof(day.highLabel)) || !containsNull(day.lowLabel, sizeof(day.lowLabel)) ||
        !containsNull(day.precipitationLabel, sizeof(day.precipitationLabel))) {
      return false;
    }
  }
  return true;
}

}  // namespace phone_sync
