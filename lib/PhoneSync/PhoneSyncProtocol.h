#pragma once

#include <cstddef>
#include <cstdint>

namespace phone_sync {

inline constexpr uint8_t PROTOCOL_VERSION = 1;
inline constexpr uint8_t WEATHER_PROTOCOL_VERSION = 1;
inline constexpr size_t MAX_EVENTS = 8;
inline constexpr size_t MAX_FORECAST_DAYS = 5;
inline constexpr char SNAPSHOT_MAGIC[4] = {'X', '3', 'C', 'L'};
inline constexpr char WEATHER_SNAPSHOT_MAGIC[4] = {'X', '3', 'W', 'T'};

inline constexpr char SERVICE_UUID[] = "7d2ea28a-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char CONTROL_UUID[] = "7d2ea28b-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char DATA_UUID[] = "7d2ea28c-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char STATUS_UUID[] = "7d2ea28d-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char WEATHER_CONTROL_UUID[] = "7d2ea28e-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char WEATHER_DATA_UUID[] = "7d2ea28f-f7bd-485a-bd9d-92ad6ecfe93e";
inline constexpr char WEATHER_STATUS_UUID[] = "7d2ea290-f7bd-485a-bd9d-92ad6ecfe93e";

enum class EventFlags : uint8_t { None = 0, AllDay = 1 << 0 };

#pragma pack(push, 1)
struct CalendarEvent {
  uint8_t flags;
  uint8_t reserved[3];
  uint32_t startEpoch;
  uint32_t endEpoch;
  char startLabel[16];
  char endLabel[16];
  char title[64];
  char location[40];
};

struct CalendarSnapshot {
  char magic[4];
  uint8_t version;
  uint8_t eventCount;
  uint16_t wireSize;
  uint32_t sequence;
  uint32_t generatedAt;
  uint32_t validUntil;
  char dateLabel[40];
  CalendarEvent events[MAX_EVENTS];
  uint32_t crc32;
};

struct WeatherDay {
  char dayLabel[12];
  char condition[24];
  char highLabel[10];
  char lowLabel[10];
  char precipitationLabel[10];
};

struct WeatherSnapshot {
  char magic[4];
  uint8_t version;
  uint8_t dayCount;
  uint16_t wireSize;
  uint32_t sequence;
  uint32_t generatedAt;
  uint32_t validUntil;
  char location[40];
  char condition[32];
  char temperature[12];
  char apparentTemperature[12];
  char humidity[12];
  char wind[16];
  WeatherDay days[MAX_FORECAST_DAYS];
  uint32_t crc32;
};

enum class ControlOpcode : uint8_t { Begin = 1, Commit = 2, Cancel = 3 };

struct BeginCommand {
  uint8_t opcode;
  uint16_t wireSize;
};

enum class SyncState : uint8_t {
  Stopped = 0,
  Advertising = 1,
  Connected = 2,
  Pairing = 3,
  Receiving = 4,
  SnapshotReady = 5,
  Accepted = 6,
  Error = 7,
};

enum class SyncError : uint8_t {
  None = 0,
  InvalidCommand = 1,
  InvalidLength = 2,
  UnexpectedOffset = 3,
  InvalidSnapshot = 4,
  StorageFailure = 5,
  StaleSnapshot = 6,
  RadioFailure = 7,
};

struct Status {
  uint8_t protocolVersion;
  uint8_t state;
  uint8_t error;
  uint8_t reserved;
  uint16_t receivedBytes;
  uint16_t expectedBytes;
  uint32_t acceptedSequence;
};
#pragma pack(pop)

static_assert(sizeof(CalendarEvent) == 148);
static_assert(sizeof(CalendarSnapshot) == 1248);
static_assert(sizeof(WeatherDay) == 66);
static_assert(sizeof(WeatherSnapshot) == 478);
static_assert(sizeof(BeginCommand) == 3);
static_assert(sizeof(Status) == 12);

uint32_t crc32(const uint8_t* data, size_t length);
uint32_t snapshotCrc32(const CalendarSnapshot& snapshot);
bool validateSnapshot(const CalendarSnapshot& snapshot);
uint32_t weatherSnapshotCrc32(const WeatherSnapshot& snapshot);
bool validateWeatherSnapshot(const WeatherSnapshot& snapshot);

}  // namespace phone_sync
