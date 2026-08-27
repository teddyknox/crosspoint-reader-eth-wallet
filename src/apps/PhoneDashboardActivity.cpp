#include "PhoneDashboardActivity.h"

#include <GfxRenderer.h>
#include <HalBlePhoneSync.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "SystemSleep.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "phone/PhoneSnapshotStore.h"
#include "phone/PhoneSyncConfig.h"
#include "phone/WeatherSnapshotStore.h"

PhoneDashboardActivity::PhoneDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool automaticWake, const bool displayOnly)
    : Activity("PhoneDashboard", renderer, mappedInput), automaticWake(automaticWake), displayOnly(displayOnly) {}

void PhoneDashboardActivity::startSyncWindow() {
  startedAt = millis();
  windowExpired = false;
  radioStarted = PHONE_SYNC_BLE.begin();
  lastState = PHONE_SYNC_BLE.state();
  lastPasskey = PHONE_SYNC_BLE.pairingPasskey();
}

void PhoneDashboardActivity::onEnter() {
  Activity::onEnter();
  hasSnapshot = PHONE_SNAPSHOT_STORE.copySnapshot(snapshot);
  if (!displayOnly) startSyncWindow();
  // A timer wake keeps the retained e-ink frame untouched until genuinely new
  // calendar data arrives. Manual entry paints the cached dashboard and status.
  if (!automaticWake) requestUpdate();
}

void PhoneDashboardActivity::onExit() {
  PHONE_SYNC_BLE.end();
  Activity::onExit();
}

bool PhoneDashboardActivity::preventAutoSleep() { return PHONE_SYNC_BLE.isActive(); }

void PhoneDashboardActivity::finishAutomaticSync(const bool repaint) {
  if (repaint) requestUpdateAndWait();
  automaticSleepAt = millis() + phone_sync::SUCCESS_DISPLAY_MS;
}

void PhoneDashboardActivity::loop() {
  if (displayOnly) return;
  if (PHONE_SYNC_BLE.takeWeatherSnapshot(weatherIncoming)) {
    const auto weatherResult = WEATHER_SNAPSHOT_STORE.save(weatherIncoming);
    const bool accepted = weatherResult != phone_sync::WeatherSnapshotStore::SaveResult::Error &&
                          weatherResult != phone_sync::WeatherSnapshotStore::SaveResult::Stale;
    const auto error = weatherResult == phone_sync::WeatherSnapshotStore::SaveResult::Stale
                           ? phone_sync::SyncError::StaleSnapshot
                       : accepted ? phone_sync::SyncError::None
                                  : phone_sync::SyncError::StorageFailure;
    PHONE_SYNC_BLE.acknowledgeWeather(weatherIncoming.sequence, accepted, error);
  }
  phone_sync::CalendarSnapshot incoming{};
  if (PHONE_SYNC_BLE.takeSnapshot(incoming)) {
    const auto result = PHONE_SNAPSHOT_STORE.save(incoming);
    switch (result) {
      case phone_sync::PhoneSnapshotStore::SaveResult::Updated: {
        {
          RenderLock lock(*this);
          snapshot = incoming;
          hasSnapshot = true;
        }
        PHONE_SYNC_BLE.acknowledge(incoming.sequence, true);
        if (automaticWake) {
          finishAutomaticSync(true);
        } else {
          requestUpdate();
        }
        break;
      }
      case phone_sync::PhoneSnapshotStore::SaveResult::Unchanged:
        PHONE_SYNC_BLE.acknowledge(incoming.sequence, true);
        if (automaticWake) finishAutomaticSync(false);
        break;
      case phone_sync::PhoneSnapshotStore::SaveResult::Stale:
        PHONE_SYNC_BLE.acknowledge(incoming.sequence, false, phone_sync::SyncError::StaleSnapshot);
        if (automaticWake) finishAutomaticSync(false);
        break;
      case phone_sync::PhoneSnapshotStore::SaveResult::Error:
        PHONE_SYNC_BLE.acknowledge(incoming.sequence, false, phone_sync::SyncError::StorageFailure);
        if (automaticWake) finishAutomaticSync(false);
        break;
    }
  }

  if (automaticSleepAt != 0 && static_cast<int32_t>(millis() - automaticSleepAt) >= 0) {
    PHONE_SYNC_BLE.end();
    enterDeepSleep(true, true);
  }

  const uint32_t windowMs = automaticWake ? phone_sync::AUTOMATIC_SYNC_WINDOW_MS : phone_sync::MANUAL_SYNC_WINDOW_MS;
  if (!windowExpired && millis() - startedAt >= windowMs) {
    windowExpired = true;
    PHONE_SYNC_BLE.end();
    if (automaticWake) enterDeepSleep(true, true);
    requestUpdate();
  }

  if (!automaticWake) {
    const auto currentState = PHONE_SYNC_BLE.state();
    const uint32_t currentPasskey = PHONE_SYNC_BLE.pairingPasskey();
    if (currentState != lastState || currentPasskey != lastPasskey) {
      lastState = currentState;
      lastPasskey = currentPasskey;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !PHONE_SYNC_BLE.isActive()) {
      startSyncWindow();
      requestUpdate();
    }
  }
}

const char* PhoneDashboardActivity::statusText() const {
  if (displayOnly) return "";
  if (windowExpired) return tr(STR_PHONE_SYNC_RETRY);
  switch (PHONE_SYNC_BLE.state()) {
    case phone_sync::SyncState::Advertising:
      return tr(STR_PHONE_SYNC_ADVERTISING);
    case phone_sync::SyncState::Connected:
      return tr(STR_PHONE_SYNC_CONNECTED);
    case phone_sync::SyncState::Pairing:
      return tr(STR_PHONE_SYNC_PAIRING);
    case phone_sync::SyncState::Receiving:
    case phone_sync::SyncState::SnapshotReady:
      return tr(STR_PHONE_SYNC_RECEIVING);
    case phone_sync::SyncState::Accepted:
      return tr(STR_PHONE_SYNC_UPDATED);
    case phone_sync::SyncState::Error:
      return tr(STR_PHONE_SYNC_FAILED);
    case phone_sync::SyncState::Stopped:
    default:
      return radioStarted ? tr(STR_PHONE_SYNC_RETRY) : tr(STR_PHONE_SYNC_FAILED);
  }
}

void PhoneDashboardActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DAILY_CALENDAR));
  int y = metrics.topPadding + metrics.headerHeight;
  if (hasSnapshot) {
    GUI.drawSubHeader(renderer, Rect{0, y, pageWidth, metrics.tabBarHeight}, snapshot.dateLabel, statusText());
    y += metrics.tabBarHeight + metrics.verticalSpacing * 2;

    const int timeWidth = 94;
    const int textX = metrics.contentSidePadding + timeWidth;
    const int availableWidth = pageWidth - textX - metrics.contentSidePadding;
    const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int detailHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int rowHeight = titleHeight + detailHeight + metrics.verticalSpacing * 3;
    for (size_t i = 0; i < snapshot.eventCount && y + rowHeight < pageHeight - metrics.buttonHintsHeight; ++i) {
      const auto& event = snapshot.events[i];
      const char* timeLabel = (event.flags & static_cast<uint8_t>(phone_sync::EventFlags::AllDay)) != 0
                                  ? tr(STR_ALL_DAY)
                                  : event.startLabel;
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, timeLabel, true, EpdFontFamily::BOLD);
      const std::string title = renderer.truncatedText(UI_10_FONT_ID, event.title, availableWidth, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, textX, y, title.c_str(), true, EpdFontFamily::BOLD);
      if (event.location[0] != '\0') {
        const std::string location =
            renderer.truncatedText(SMALL_FONT_ID, event.location, availableWidth, EpdFontFamily::REGULAR);
        renderer.drawText(SMALL_FONT_ID, textX, y + titleHeight + metrics.verticalSpacing, location.c_str());
      } else if ((event.flags & static_cast<uint8_t>(phone_sync::EventFlags::AllDay)) == 0 &&
                 event.endLabel[0] != '\0') {
        renderer.drawText(SMALL_FONT_ID, textX, y + titleHeight + metrics.verticalSpacing, event.endLabel);
      }
      y += rowHeight;
      renderer.drawLine(metrics.contentSidePadding, y - metrics.verticalSpacing, pageWidth - metrics.contentSidePadding,
                        y - metrics.verticalSpacing, 1, true);
    }
  } else {
    const int center = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, center - 24, tr(STR_PHONE_SYNC_NO_DATA), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, center + 8, tr(STR_PHONE_SYNC_NO_DATA_HINT), true);
  }

  const uint32_t passkey = PHONE_SYNC_BLE.pairingPasskey();
  if (passkey != 0) {
    char passkeyText[48];
    snprintf(passkeyText, sizeof(passkeyText), tr(STR_PHONE_SYNC_PASSKEY), static_cast<unsigned long>(passkey));
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight - metrics.buttonHintsHeight - 32, passkeyText, true,
                              EpdFontFamily::BOLD);
  }

  if (!automaticWake && !displayOnly) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), PHONE_SYNC_BLE.isActive() ? "" : tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
