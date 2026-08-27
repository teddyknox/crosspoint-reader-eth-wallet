#pragma once

#include <PhoneSyncProtocol.h>

#include <cstdint>

#include "activities/Activity.h"

class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool displayOnly = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  const bool displayOnly;
  bool hasSnapshot = false;
  bool radioStarted = false;
  bool windowExpired = false;
  uint32_t startedAt = 0;
  phone_sync::SyncState lastState = phone_sync::SyncState::Stopped;
  uint32_t lastPasskey = 0;
  phone_sync::WeatherSnapshot snapshot{};

  void startSyncWindow();
  void setAsSleepScreen();
  const char* statusText() const;
};
