#include "HalBlePhoneSync.h"

#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>

#include "HalBleBondSecurity.h"

namespace {

using phone_sync::CalendarSnapshot;
using phone_sync::SyncError;
using phone_sync::SyncState;
using phone_sync::WeatherSnapshot;

std::atomic<SyncState> syncState{SyncState::Stopped};
std::atomic<SyncError> syncError{SyncError::None};
std::atomic<uint32_t> pairingPasskeyValue{0};
std::atomic<bool> active{false};
std::atomic<bool> snapshotReady{false};
std::atomic<uint16_t> expectedBytes{0};
std::atomic<uint16_t> receivedBytes{0};
std::atomic<uint32_t> acceptedSequence{0};
std::atomic<SyncState> weatherSyncState{SyncState::Stopped};
std::atomic<SyncError> weatherSyncError{SyncError::None};
std::atomic<bool> weatherSnapshotReady{false};
std::atomic<uint16_t> weatherExpectedBytes{0};
std::atomic<uint16_t> weatherReceivedBytes{0};
std::atomic<uint32_t> acceptedWeatherSequence{0};

portMUX_TYPE stagingMux = portMUX_INITIALIZER_UNLOCKED;
CalendarSnapshot stagingSnapshot{};
portMUX_TYPE weatherStagingMux = portMUX_INITIALIZER_UNLOCKED;
WeatherSnapshot stagingWeatherSnapshot{};

NimBLEServer* server = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;
NimBLECharacteristic* weatherStatusCharacteristic = nullptr;

void publishStatus(const bool notify = true) {
  if (!statusCharacteristic) return;
  const phone_sync::Status status{
      phone_sync::PROTOCOL_VERSION,
      static_cast<uint8_t>(syncState.load(std::memory_order_relaxed)),
      static_cast<uint8_t>(syncError.load(std::memory_order_relaxed)),
      0,
      receivedBytes.load(std::memory_order_relaxed),
      expectedBytes.load(std::memory_order_relaxed),
      acceptedSequence.load(std::memory_order_relaxed),
  };
  statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
  if (notify && server && server->getConnectedCount() > 0) statusCharacteristic->notify();
}

void publishWeatherStatus(const bool notify = true) {
  if (!weatherStatusCharacteristic) return;
  const phone_sync::Status status{
      phone_sync::WEATHER_PROTOCOL_VERSION,
      static_cast<uint8_t>(weatherSyncState.load(std::memory_order_relaxed)),
      static_cast<uint8_t>(weatherSyncError.load(std::memory_order_relaxed)),
      0,
      weatherReceivedBytes.load(std::memory_order_relaxed),
      weatherExpectedBytes.load(std::memory_order_relaxed),
      acceptedWeatherSequence.load(std::memory_order_relaxed),
  };
  weatherStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
  if (notify && server && server->getConnectedCount() > 0) weatherStatusCharacteristic->notify();
}

void fail(const SyncError error) {
  syncError.store(error, std::memory_order_relaxed);
  syncState.store(SyncState::Error, std::memory_order_relaxed);
  snapshotReady.store(false, std::memory_order_release);
  publishStatus();
}

void failWeather(const SyncError error) {
  weatherSyncError.store(error, std::memory_order_relaxed);
  weatherSyncState.store(SyncState::Error, std::memory_order_relaxed);
  weatherSnapshotReady.store(false, std::memory_order_release);
  publishWeatherStatus();
}

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8U);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* connectedServer, NimBLEConnInfo& connection) override {
    syncState.store(SyncState::Connected, std::memory_order_relaxed);
    syncError.store(SyncError::None, std::memory_order_relaxed);
    weatherSyncState.store(SyncState::Connected, std::memory_order_relaxed);
    weatherSyncError.store(SyncError::None, std::memory_order_relaxed);
    connectedServer->updateConnParams(connection.getConnHandle(), 12, 24, 0, 400);
    publishStatus();
    publishWeatherStatus();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    pairingPasskeyValue.store(0, std::memory_order_relaxed);
    if (!active.load(std::memory_order_relaxed)) return;
    syncState.store(SyncState::Advertising, std::memory_order_relaxed);
    weatherSyncState.store(SyncState::Advertising, std::memory_order_relaxed);
    publishStatus(false);
    publishWeatherStatus(false);
    NimBLEDevice::startAdvertising();
  }

  uint32_t onPassKeyDisplay() override {
    const uint32_t passkey = 100000U + (esp_random() % 900000U);
    pairingPasskeyValue.store(passkey, std::memory_order_relaxed);
    syncState.store(SyncState::Pairing, std::memory_order_relaxed);
    weatherSyncState.store(SyncState::Pairing, std::memory_order_relaxed);
    publishStatus();
    publishWeatherStatus();
    return passkey;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connection) override {
    pairingPasskeyValue.store(0, std::memory_order_relaxed);
    if (!hal_ble_bond::authorizeOrAdopt(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(SyncError::RadioFailure);
      failWeather(SyncError::RadioFailure);
      return;
    }
    syncState.store(SyncState::Connected, std::memory_order_relaxed);
    syncError.store(SyncError::None, std::memory_order_relaxed);
    weatherSyncState.store(SyncState::Connected, std::memory_order_relaxed);
    weatherSyncError.store(SyncError::None, std::memory_order_relaxed);
    publishStatus();
    publishWeatherStatus();
  }
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(SyncError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (value.size() < 1) {
      fail(SyncError::InvalidCommand);
      return;
    }

    const auto opcode = static_cast<phone_sync::ControlOpcode>(value.data()[0]);
    if (opcode == phone_sync::ControlOpcode::Begin) {
      if (value.size() != sizeof(phone_sync::BeginCommand)) {
        fail(SyncError::InvalidCommand);
        return;
      }
      const uint16_t wireSize = readU16(value.data() + 1);
      if (wireSize != sizeof(CalendarSnapshot)) {
        fail(SyncError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&stagingMux);
      std::memset(&stagingSnapshot, 0, sizeof(stagingSnapshot));
      taskEXIT_CRITICAL(&stagingMux);
      snapshotReady.store(false, std::memory_order_release);
      expectedBytes.store(wireSize, std::memory_order_relaxed);
      receivedBytes.store(0, std::memory_order_relaxed);
      syncError.store(SyncError::None, std::memory_order_relaxed);
      syncState.store(SyncState::Receiving, std::memory_order_relaxed);
      publishStatus();
      return;
    }

    if (opcode == phone_sync::ControlOpcode::Commit) {
      if (value.size() != 1 || syncState.load(std::memory_order_relaxed) != SyncState::Receiving ||
          receivedBytes.load(std::memory_order_relaxed) != expectedBytes.load(std::memory_order_relaxed)) {
        fail(SyncError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&stagingMux);
      const bool valid = phone_sync::validateSnapshot(stagingSnapshot);
      taskEXIT_CRITICAL(&stagingMux);
      if (!valid) {
        fail(SyncError::InvalidSnapshot);
        return;
      }
      snapshotReady.store(true, std::memory_order_release);
      syncState.store(SyncState::SnapshotReady, std::memory_order_relaxed);
      publishStatus();
      return;
    }

    if (opcode == phone_sync::ControlOpcode::Cancel && value.size() == 1) {
      snapshotReady.store(false, std::memory_order_release);
      expectedBytes.store(0, std::memory_order_relaxed);
      receivedBytes.store(0, std::memory_order_relaxed);
      syncState.store(SyncState::Connected, std::memory_order_relaxed);
      syncError.store(SyncError::None, std::memory_order_relaxed);
      publishStatus();
      return;
    }

    fail(SyncError::InvalidCommand);
  }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(SyncError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (syncState.load(std::memory_order_relaxed) != SyncState::Receiving || value.size() <= 2) {
      fail(SyncError::InvalidCommand);
      return;
    }

    const uint16_t offset = readU16(value.data());
    const size_t chunkSize = value.size() - 2;
    const uint16_t received = receivedBytes.load(std::memory_order_relaxed);
    const uint16_t expected = expectedBytes.load(std::memory_order_relaxed);
    if (offset != received || static_cast<size_t>(offset) + chunkSize > expected) {
      fail(SyncError::UnexpectedOffset);
      return;
    }

    taskENTER_CRITICAL(&stagingMux);
    std::memcpy(reinterpret_cast<uint8_t*>(&stagingSnapshot) + offset, value.data() + 2, chunkSize);
    taskEXIT_CRITICAL(&stagingMux);
    receivedBytes.store(static_cast<uint16_t>(received + chunkSize), std::memory_order_relaxed);
    publishStatus();
  }
};

class WeatherControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      failWeather(SyncError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (value.size() < 1) {
      failWeather(SyncError::InvalidCommand);
      return;
    }

    const auto opcode = static_cast<phone_sync::ControlOpcode>(value.data()[0]);
    if (opcode == phone_sync::ControlOpcode::Begin) {
      if (value.size() != sizeof(phone_sync::BeginCommand)) {
        failWeather(SyncError::InvalidCommand);
        return;
      }
      const uint16_t wireSize = readU16(value.data() + 1);
      if (wireSize != sizeof(WeatherSnapshot)) {
        failWeather(SyncError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&weatherStagingMux);
      std::memset(&stagingWeatherSnapshot, 0, sizeof(stagingWeatherSnapshot));
      taskEXIT_CRITICAL(&weatherStagingMux);
      weatherSnapshotReady.store(false, std::memory_order_release);
      weatherExpectedBytes.store(wireSize, std::memory_order_relaxed);
      weatherReceivedBytes.store(0, std::memory_order_relaxed);
      weatherSyncError.store(SyncError::None, std::memory_order_relaxed);
      weatherSyncState.store(SyncState::Receiving, std::memory_order_relaxed);
      publishWeatherStatus();
      return;
    }

    if (opcode == phone_sync::ControlOpcode::Commit) {
      if (value.size() != 1 || weatherSyncState.load(std::memory_order_relaxed) != SyncState::Receiving ||
          weatherReceivedBytes.load(std::memory_order_relaxed) !=
              weatherExpectedBytes.load(std::memory_order_relaxed)) {
        failWeather(SyncError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&weatherStagingMux);
      const bool valid = phone_sync::validateWeatherSnapshot(stagingWeatherSnapshot);
      taskEXIT_CRITICAL(&weatherStagingMux);
      if (!valid) {
        failWeather(SyncError::InvalidSnapshot);
        return;
      }
      weatherSnapshotReady.store(true, std::memory_order_release);
      weatherSyncState.store(SyncState::SnapshotReady, std::memory_order_relaxed);
      publishWeatherStatus();
      return;
    }

    if (opcode == phone_sync::ControlOpcode::Cancel && value.size() == 1) {
      weatherSnapshotReady.store(false, std::memory_order_release);
      weatherExpectedBytes.store(0, std::memory_order_relaxed);
      weatherReceivedBytes.store(0, std::memory_order_relaxed);
      weatherSyncState.store(SyncState::Connected, std::memory_order_relaxed);
      weatherSyncError.store(SyncError::None, std::memory_order_relaxed);
      publishWeatherStatus();
      return;
    }

    failWeather(SyncError::InvalidCommand);
  }
};

class WeatherDataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      failWeather(SyncError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (weatherSyncState.load(std::memory_order_relaxed) != SyncState::Receiving || value.size() <= 2) {
      failWeather(SyncError::InvalidCommand);
      return;
    }

    const uint16_t offset = readU16(value.data());
    const size_t chunkSize = value.size() - 2;
    const uint16_t received = weatherReceivedBytes.load(std::memory_order_relaxed);
    const uint16_t expected = weatherExpectedBytes.load(std::memory_order_relaxed);
    if (offset != received || static_cast<size_t>(offset) + chunkSize > expected) {
      failWeather(SyncError::UnexpectedOffset);
      return;
    }

    taskENTER_CRITICAL(&weatherStagingMux);
    std::memcpy(reinterpret_cast<uint8_t*>(&stagingWeatherSnapshot) + offset, value.data() + 2, chunkSize);
    taskEXIT_CRITICAL(&weatherStagingMux);
    weatherReceivedBytes.store(static_cast<uint16_t>(received + chunkSize), std::memory_order_relaxed);
    publishWeatherStatus();
  }
};

ServerCallbacks serverCallbacks;
ControlCallbacks controlCallbacks;
DataCallbacks dataCallbacks;
WeatherControlCallbacks weatherControlCallbacks;
WeatherDataCallbacks weatherDataCallbacks;

}  // namespace

HalBlePhoneSync& HalBlePhoneSync::getInstance() {
  static HalBlePhoneSync instance;
  return instance;
}

bool HalBlePhoneSync::begin() {
  if (active.load(std::memory_order_relaxed)) return true;
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);

  syncState.store(SyncState::Stopped, std::memory_order_relaxed);
  syncError.store(SyncError::None, std::memory_order_relaxed);
  pairingPasskeyValue.store(0, std::memory_order_relaxed);
  snapshotReady.store(false, std::memory_order_relaxed);
  expectedBytes.store(0, std::memory_order_relaxed);
  receivedBytes.store(0, std::memory_order_relaxed);
  weatherSyncState.store(SyncState::Stopped, std::memory_order_relaxed);
  weatherSyncError.store(SyncError::None, std::memory_order_relaxed);
  weatherSnapshotReady.store(false, std::memory_order_relaxed);
  weatherExpectedBytes.store(0, std::memory_order_relaxed);
  weatherReceivedBytes.store(0, std::memory_order_relaxed);

  if (!NimBLEDevice::init("X3 Phone Dashboard")) {
    fail(SyncError::RadioFailure);
    return false;
  }
  if (!hal_ble_bond::initialize()) {
    NimBLEDevice::deinit(true);
    fail(SyncError::RadioFailure);
    return false;
  }
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*secureConnections=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  server = NimBLEDevice::createServer();
  if (!server) {
    NimBLEDevice::deinit(true);
    fail(SyncError::RadioFailure);
    return false;
  }
  // The callbacks have static storage duration. NimBLE owns callbacks by
  // default and would otherwise delete this object during deinit().
  server->setCallbacks(&serverCallbacks, false);

  NimBLEService* service = server->createService(phone_sync::SERVICE_UUID);
  if (!service) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    fail(SyncError::RadioFailure);
    return false;
  }

  auto* control = service->createCharacteristic(
      phone_sync::CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, sizeof(phone_sync::BeginCommand));
  auto* data =
      service->createCharacteristic(phone_sync::DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 244);
  statusCharacteristic = service->createCharacteristic(
      phone_sync::STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY,
      sizeof(phone_sync::Status));
  auto* weatherControl = service->createCharacteristic(phone_sync::WEATHER_CONTROL_UUID,
                                                       NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
                                                       sizeof(phone_sync::BeginCommand));
  auto* weatherData = service->createCharacteristic(phone_sync::WEATHER_DATA_UUID,
                                                    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 244);
  weatherStatusCharacteristic = service->createCharacteristic(
      phone_sync::WEATHER_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY,
      sizeof(phone_sync::Status));
  if (!control || !data || !statusCharacteristic || !weatherControl || !weatherData || !weatherStatusCharacteristic) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    weatherStatusCharacteristic = nullptr;
    fail(SyncError::RadioFailure);
    failWeather(SyncError::RadioFailure);
    return false;
  }
  control->setCallbacks(&controlCallbacks);
  data->setCallbacks(&dataCallbacks);
  weatherControl->setCallbacks(&weatherControlCallbacks);
  weatherData->setCallbacks(&weatherDataCallbacks);
  publishStatus(false);
  publishWeatherStatus(false);

  if (!service->start()) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    weatherStatusCharacteristic = nullptr;
    fail(SyncError::RadioFailure);
    failWeather(SyncError::RadioFailure);
    return false;
  }

  auto* advertising = NimBLEDevice::getAdvertising();
  // Keep the 128-bit service UUID in the primary advertising packet so iOS can
  // wake a suspended, screen-off central from its service-filtered scan. Put
  // the longer display name in the scan response instead.
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(service->getUUID());
  advertising->setName("X3 Phone Dashboard");
  hal_ble_bond::restrictAdvertising(*advertising);
  if (!advertising->start()) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    weatherStatusCharacteristic = nullptr;
    fail(SyncError::RadioFailure);
    failWeather(SyncError::RadioFailure);
    return false;
  }

  active.store(true, std::memory_order_relaxed);
  syncState.store(SyncState::Advertising, std::memory_order_relaxed);
  weatherSyncState.store(SyncState::Advertising, std::memory_order_relaxed);
  publishStatus(false);
  publishWeatherStatus(false);
  LOG_INF("BLE", "Phone sync advertising started");
  return true;
}

void HalBlePhoneSync::end() {
  if (!active.exchange(false, std::memory_order_relaxed) && !NimBLEDevice::isInitialized()) return;
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(true);
  }
  server = nullptr;
  statusCharacteristic = nullptr;
  weatherStatusCharacteristic = nullptr;
  pairingPasskeyValue.store(0, std::memory_order_relaxed);
  snapshotReady.store(false, std::memory_order_relaxed);
  weatherSnapshotReady.store(false, std::memory_order_relaxed);
  syncState.store(SyncState::Stopped, std::memory_order_relaxed);
  weatherSyncState.store(SyncState::Stopped, std::memory_order_relaxed);
  LOG_INF("BLE", "Phone sync stopped");
}

bool HalBlePhoneSync::takeSnapshot(CalendarSnapshot& destination) {
  if (!snapshotReady.exchange(false, std::memory_order_acq_rel)) return false;
  taskENTER_CRITICAL(&stagingMux);
  destination = stagingSnapshot;
  taskEXIT_CRITICAL(&stagingMux);
  return true;
}

bool HalBlePhoneSync::takeWeatherSnapshot(WeatherSnapshot& destination) {
  if (!weatherSnapshotReady.exchange(false, std::memory_order_acq_rel)) return false;
  taskENTER_CRITICAL(&weatherStagingMux);
  destination = stagingWeatherSnapshot;
  taskEXIT_CRITICAL(&weatherStagingMux);
  return true;
}

void HalBlePhoneSync::acknowledge(const uint32_t sequence, const bool accepted, const SyncError error) {
  if (accepted) acceptedSequence.store(sequence, std::memory_order_relaxed);
  syncError.store(error, std::memory_order_relaxed);
  syncState.store(accepted ? SyncState::Accepted : SyncState::Error, std::memory_order_relaxed);
  publishStatus();
}

void HalBlePhoneSync::acknowledgeWeather(const uint32_t sequence, const bool accepted, const SyncError error) {
  if (accepted) acceptedWeatherSequence.store(sequence, std::memory_order_relaxed);
  weatherSyncError.store(error, std::memory_order_relaxed);
  weatherSyncState.store(accepted ? SyncState::Accepted : SyncState::Error, std::memory_order_relaxed);
  publishWeatherStatus();
}

SyncState HalBlePhoneSync::state() const { return syncState.load(std::memory_order_relaxed); }

SyncError HalBlePhoneSync::error() const { return syncError.load(std::memory_order_relaxed); }

SyncState HalBlePhoneSync::weatherState() const { return weatherSyncState.load(std::memory_order_relaxed); }

SyncError HalBlePhoneSync::weatherError() const { return weatherSyncError.load(std::memory_order_relaxed); }

uint32_t HalBlePhoneSync::pairingPasskey() const { return pairingPasskeyValue.load(std::memory_order_relaxed); }

bool HalBlePhoneSync::isActive() const { return active.load(std::memory_order_relaxed); }
