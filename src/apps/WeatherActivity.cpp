#include "WeatherActivity.h"

#include <GfxRenderer.h>
#include <HalBlePhoneSync.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "phone/PhoneSyncConfig.h"
#include "phone/WeatherSnapshotStore.h"

WeatherActivity::WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool displayOnly)
    : Activity("Weather", renderer, mappedInput), displayOnly(displayOnly) {}

void WeatherActivity::startSyncWindow() {
  startedAt = millis();
  windowExpired = false;
  radioStarted = PHONE_SYNC_BLE.begin();
  lastState = PHONE_SYNC_BLE.weatherState();
  lastPasskey = PHONE_SYNC_BLE.pairingPasskey();
}

void WeatherActivity::onEnter() {
  Activity::onEnter();
  hasSnapshot = WEATHER_SNAPSHOT_STORE.copySnapshot(snapshot);
  if (!displayOnly) startSyncWindow();
  requestUpdate();
}

void WeatherActivity::onExit() {
  PHONE_SYNC_BLE.end();
  Activity::onExit();
}

bool WeatherActivity::preventAutoSleep() { return !displayOnly && PHONE_SYNC_BLE.isActive(); }

void WeatherActivity::setAsSleepScreen() {
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::APP_WEATHER) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::APP_WEATHER;
    SETTINGS.saveToFile();
  }
  requestUpdate();
}

void WeatherActivity::loop() {
  if (displayOnly) return;
  if (PHONE_SYNC_BLE.takeWeatherSnapshot(snapshot)) {
    const auto result = WEATHER_SNAPSHOT_STORE.save(snapshot);
    const bool accepted = result != phone_sync::WeatherSnapshotStore::SaveResult::Error &&
                          result != phone_sync::WeatherSnapshotStore::SaveResult::Stale;
    const auto error = result == phone_sync::WeatherSnapshotStore::SaveResult::Stale
                           ? phone_sync::SyncError::StaleSnapshot
                       : accepted ? phone_sync::SyncError::None
                                  : phone_sync::SyncError::StorageFailure;
    PHONE_SYNC_BLE.acknowledgeWeather(snapshot.sequence, accepted, error);
    if (accepted) hasSnapshot = true;
    requestUpdate();
  }

  if (!windowExpired && millis() - startedAt >= phone_sync::MANUAL_SYNC_WINDOW_MS) {
    windowExpired = true;
    PHONE_SYNC_BLE.end();
    requestUpdate();
  }

  const auto currentState = PHONE_SYNC_BLE.weatherState();
  const uint32_t currentPasskey = PHONE_SYNC_BLE.pairingPasskey();
  if (currentState != lastState || currentPasskey != lastPasskey) {
    lastState = currentState;
    lastPasskey = currentPasskey;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    setAsSleepScreen();
    return;
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

const char* WeatherActivity::statusText() const {
  if (displayOnly) return "";
  if (windowExpired) return tr(STR_PHONE_SYNC_RETRY);
  switch (PHONE_SYNC_BLE.weatherState()) {
    case phone_sync::SyncState::Advertising:
      return tr(STR_PHONE_SYNC_ADVERTISING);
    case phone_sync::SyncState::Connected:
      return tr(STR_PHONE_SYNC_CONNECTED);
    case phone_sync::SyncState::Pairing:
      return tr(STR_PHONE_SYNC_PAIRING);
    case phone_sync::SyncState::Receiving:
    case phone_sync::SyncState::SnapshotReady:
      return tr(STR_WEATHER_SYNC_RECEIVING);
    case phone_sync::SyncState::Accepted:
      return tr(STR_WEATHER_SYNC_UPDATED);
    case phone_sync::SyncState::Error:
      return tr(STR_PHONE_SYNC_FAILED);
    case phone_sync::SyncState::Stopped:
    default:
      return radioStarted ? tr(STR_PHONE_SYNC_RETRY) : tr(STR_PHONE_SYNC_FAILED);
  }
}

void WeatherActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER));
  int y = metrics.topPadding + metrics.headerHeight;
  if (hasSnapshot) {
    GUI.drawSubHeader(renderer, Rect{0, y, pageWidth, metrics.tabBarHeight}, snapshot.location, statusText());
    y += metrics.tabBarHeight + metrics.verticalSpacing * 2;

    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, snapshot.temperature, true, EpdFontFamily::BOLD);
    const int conditionX = metrics.contentSidePadding + 112;
    const int conditionWidth = pageWidth - conditionX - metrics.contentSidePadding;
    const std::string condition =
        renderer.truncatedText(UI_12_FONT_ID, snapshot.condition, conditionWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, conditionX, y, condition.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    char details[96];
    snprintf(details, sizeof(details), "%s %s   %s %s   %s %s", tr(STR_FEELS_LIKE), snapshot.apparentTemperature,
             tr(STR_HUMIDITY), snapshot.humidity, tr(STR_WIND), snapshot.wind);
    const std::string detailText = renderer.truncatedText(
        SMALL_FONT_ID, details, pageWidth - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, detailText.c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 3;

    const int dayWidth = 72;
    const int temperaturesWidth = 122;
    const int conditionWidthForecast = pageWidth - metrics.contentSidePadding * 2 - dayWidth - temperaturesWidth;
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 3;
    for (size_t i = 0; i < snapshot.dayCount && y + rowHeight < pageHeight - metrics.buttonHintsHeight; ++i) {
      const auto& day = snapshot.days[i];
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, day.dayLabel, true, EpdFontFamily::BOLD);
      char forecast[48];
      snprintf(forecast, sizeof(forecast), "%s  %s", day.condition, day.precipitationLabel);
      const std::string dayCondition =
          renderer.truncatedText(UI_10_FONT_ID, forecast, conditionWidthForecast, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + dayWidth, y, dayCondition.c_str());

      char temperatures[40];
      snprintf(temperatures, sizeof(temperatures), "%s  %s", day.highLabel, day.lowLabel);
      renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - temperaturesWidth, y, temperatures);
      y += rowHeight;
      renderer.drawLine(metrics.contentSidePadding, y - metrics.verticalSpacing, pageWidth - metrics.contentSidePadding,
                        y - metrics.verticalSpacing, 1, true);
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pageHeight - metrics.buttonHintsHeight - 22,
                      tr(STR_WEATHER_ATTRIBUTION));
  } else {
    const int center = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, center - 24, tr(STR_WEATHER_NO_DATA), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, center + 8, tr(STR_WEATHER_NO_DATA_HINT), true);
  }

  const uint32_t passkey = PHONE_SYNC_BLE.pairingPasskey();
  if (passkey != 0) {
    char passkeyText[48];
    snprintf(passkeyText, sizeof(passkeyText), tr(STR_PHONE_SYNC_PASSKEY), static_cast<unsigned long>(passkey));
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight - metrics.buttonHintsHeight - 32, passkeyText, true,
                              EpdFontFamily::BOLD);
  }

  if (!displayOnly) {
    const char* sleepLabel = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::APP_WEATHER
                                 ? tr(STR_SELECTED)
                                 : tr(STR_SET_SLEEP_SCREEN);
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), PHONE_SYNC_BLE.isActive() ? "" : tr(STR_RETRY), "", sleepLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
