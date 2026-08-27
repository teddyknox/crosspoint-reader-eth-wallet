#pragma once

#include <PhoneSyncProtocol.h>

#include <cstdint>

#include "activities/Activity.h"

class PhoneDashboardActivity final : public Activity {
 public:
  PhoneDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool automaticWake,
                         bool displayOnly = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  const bool automaticWake;
  const bool displayOnly;
  bool hasSnapshot = false;
  bool radioStarted = false;
  bool windowExpired = false;
  uint32_t startedAt = 0;
  uint32_t automaticSleepAt = 0;
  bool refreshSleepScreenOnAutomaticSleep = false;
  phone_sync::SyncState lastState = phone_sync::SyncState::Stopped;
  uint32_t lastPasskey = 0;
  phone_sync::CalendarSnapshot snapshot{};
  phone_sync::WeatherSnapshot weatherIncoming{};

  void startSyncWindow();
  void setAsSleepScreen();
  void finishAutomaticSync(bool refreshSleepScreen);
  const char* statusText() const;
};
