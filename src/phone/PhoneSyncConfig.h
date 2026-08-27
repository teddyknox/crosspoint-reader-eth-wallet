#pragma once

#include <cstdint>

namespace phone_sync {

// The X3 wakes briefly every fifteen minutes. iOS background BLE delivery is
// best-effort, so a missed window simply retries on the next timer wake.
inline constexpr uint32_t WAKE_INTERVAL_SECONDS = 15U * 60U;
inline constexpr uint32_t AUTOMATIC_SYNC_WINDOW_MS = 20U * 1000U;
inline constexpr uint32_t MANUAL_SYNC_WINDOW_MS = 60U * 1000U;
inline constexpr uint32_t SUCCESS_DISPLAY_MS = 1500U;

}  // namespace phone_sync
