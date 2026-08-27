#include "HalBleEvmWallet.h"

#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>

#include "HalBleBondSecurity.h"

namespace {

using evm_wallet::SignRequest;
using evm_wallet::WalletError;
using evm_wallet::WalletState;

std::atomic<WalletState> walletState{WalletState::Stopped};
std::atomic<WalletError> walletError{WalletError::None};
std::atomic<uint32_t> pairingPasskeyValue{0};
std::atomic<bool> active{false};
std::atomic<bool> requestReady{false};
std::atomic<uint16_t> expectedBytes{0};
std::atomic<uint16_t> receivedBytes{0};
std::atomic<uint32_t> currentRequestId{0};

portMUX_TYPE stagingMux = portMUX_INITIALIZER_UNLOCKED;
SignRequest stagingRequest{};
uint8_t walletAddress[20]{};
uint8_t signedDigest[32]{};
uint8_t signedResult[65]{};

NimBLEServer* server = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;

void publishStatus(const bool notify = true) {
  if (!statusCharacteristic) return;
  evm_wallet::WalletStatus status{};
  status.protocolVersion = evm_wallet::PROTOCOL_VERSION;
  status.state = static_cast<uint8_t>(walletState.load(std::memory_order_relaxed));
  status.error = static_cast<uint8_t>(walletError.load(std::memory_order_relaxed));
  status.receivedBytes = receivedBytes.load(std::memory_order_relaxed);
  status.expectedBytes = expectedBytes.load(std::memory_order_relaxed);
  status.requestId = currentRequestId.load(std::memory_order_relaxed);
  std::memcpy(status.address, walletAddress, sizeof(status.address));
  std::memcpy(status.digest, signedDigest, sizeof(status.digest));
  std::memcpy(status.signature, signedResult, sizeof(status.signature));
  statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
  if (notify && server && server->getConnectedCount() > 0) statusCharacteristic->notify();
}

void fail(const WalletError error) {
  walletError.store(error, std::memory_order_relaxed);
  walletState.store(WalletState::Error, std::memory_order_relaxed);
  requestReady.store(false, std::memory_order_release);
  publishStatus();
}

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8U);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* connectedServer, NimBLEConnInfo& connection) override {
    walletState.store(WalletState::Connected, std::memory_order_relaxed);
    walletError.store(WalletError::None, std::memory_order_relaxed);
    connectedServer->updateConnParams(connection.getConnHandle(), 12, 24, 0, 400);
    publishStatus();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    pairingPasskeyValue.store(0, std::memory_order_relaxed);
    if (!active.load(std::memory_order_relaxed)) return;
    walletState.store(WalletState::Advertising, std::memory_order_relaxed);
    publishStatus(false);
    if (!NimBLEDevice::startAdvertising()) fail(WalletError::RadioFailure);
  }

  uint32_t onPassKeyDisplay() override {
    const uint32_t passkey = 100000U + (esp_random() % 900000U);
    pairingPasskeyValue.store(passkey, std::memory_order_relaxed);
    walletState.store(WalletState::Pairing, std::memory_order_relaxed);
    publishStatus();
    return passkey;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connection) override {
    pairingPasskeyValue.store(0, std::memory_order_relaxed);
    if (!hal_ble_bond::authorizeOrAdopt(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(WalletError::RadioFailure);
      return;
    }
    walletState.store(WalletState::Connected, std::memory_order_relaxed);
    walletError.store(WalletError::None, std::memory_order_relaxed);
    publishStatus();
  }
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(WalletError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (value.size() < 1) {
      fail(WalletError::InvalidCommand);
      return;
    }
    const auto opcode = static_cast<evm_wallet::ControlOpcode>(value.data()[0]);
    if (opcode == evm_wallet::ControlOpcode::Begin) {
      if (value.size() != sizeof(evm_wallet::BeginCommand) || readU16(value.data() + 1) != sizeof(SignRequest)) {
        fail(WalletError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&stagingMux);
      std::memset(&stagingRequest, 0, sizeof(stagingRequest));
      taskEXIT_CRITICAL(&stagingMux);
      requestReady.store(false, std::memory_order_release);
      currentRequestId.store(0, std::memory_order_relaxed);
      expectedBytes.store(sizeof(SignRequest), std::memory_order_relaxed);
      receivedBytes.store(0, std::memory_order_relaxed);
      walletError.store(WalletError::None, std::memory_order_relaxed);
      walletState.store(WalletState::Receiving, std::memory_order_relaxed);
      publishStatus();
      return;
    }
    if (opcode == evm_wallet::ControlOpcode::Commit) {
      if (value.size() != 1 || walletState.load(std::memory_order_relaxed) != WalletState::Receiving ||
          receivedBytes.load(std::memory_order_relaxed) != sizeof(SignRequest)) {
        fail(WalletError::InvalidLength);
        return;
      }
      taskENTER_CRITICAL(&stagingMux);
      const bool valid = evm_wallet::validateRequest(stagingRequest);
      const uint32_t requestId = stagingRequest.requestId;
      taskEXIT_CRITICAL(&stagingMux);
      if (!valid) {
        fail(WalletError::InvalidRequest);
        return;
      }
      currentRequestId.store(requestId, std::memory_order_relaxed);
      requestReady.store(true, std::memory_order_release);
      walletState.store(WalletState::ReviewReady, std::memory_order_relaxed);
      publishStatus();
      return;
    }
    if (opcode == evm_wallet::ControlOpcode::Cancel && value.size() == 1) {
      requestReady.store(false, std::memory_order_release);
      walletState.store(WalletState::Connected, std::memory_order_relaxed);
      walletError.store(WalletError::None, std::memory_order_relaxed);
      publishStatus();
      return;
    }
    fail(WalletError::InvalidCommand);
  }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
    if (!hal_ble_bond::isAuthorized(connection)) {
      if (server) server->disconnect(connection.getConnHandle());
      fail(WalletError::RadioFailure);
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    if (walletState.load(std::memory_order_relaxed) != WalletState::Receiving || value.size() <= 2) {
      fail(WalletError::InvalidCommand);
      return;
    }
    const uint16_t offset = readU16(value.data());
    const size_t chunkSize = value.size() - 2;
    const uint16_t received = receivedBytes.load(std::memory_order_relaxed);
    if (offset != received || static_cast<size_t>(offset) + chunkSize > sizeof(SignRequest)) {
      fail(WalletError::UnexpectedOffset);
      return;
    }
    taskENTER_CRITICAL(&stagingMux);
    std::memcpy(reinterpret_cast<uint8_t*>(&stagingRequest) + offset, value.data() + 2, chunkSize);
    taskEXIT_CRITICAL(&stagingMux);
    receivedBytes.store(static_cast<uint16_t>(received + chunkSize), std::memory_order_relaxed);
    publishStatus();
  }
};

ServerCallbacks serverCallbacks;
ControlCallbacks controlCallbacks;
DataCallbacks dataCallbacks;

}  // namespace

HalBleEvmWallet& HalBleEvmWallet::getInstance() {
  static HalBleEvmWallet instance;
  return instance;
}

bool HalBleEvmWallet::begin(const uint8_t address[20]) {
  if (active.load(std::memory_order_relaxed)) return true;
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
  std::memcpy(walletAddress, address, sizeof(walletAddress));
  std::memset(signedDigest, 0, sizeof(signedDigest));
  std::memset(signedResult, 0, sizeof(signedResult));
  walletState.store(WalletState::Stopped, std::memory_order_relaxed);
  walletError.store(WalletError::None, std::memory_order_relaxed);
  pairingPasskeyValue.store(0, std::memory_order_relaxed);
  requestReady.store(false, std::memory_order_relaxed);
  expectedBytes.store(0, std::memory_order_relaxed);
  receivedBytes.store(0, std::memory_order_relaxed);
  currentRequestId.store(0, std::memory_order_relaxed);

  if (!NimBLEDevice::init("X3 EVM Wallet")) {
    fail(WalletError::RadioFailure);
    return false;
  }
  if (!hal_ble_bond::initialize()) {
    NimBLEDevice::deinit(true);
    fail(WalletError::RadioFailure);
    return false;
  }
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  server = NimBLEDevice::createServer();
  if (!server) {
    NimBLEDevice::deinit(true);
    fail(WalletError::RadioFailure);
    return false;
  }
  server->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = server->createService(evm_wallet::SERVICE_UUID);
  if (!service) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    fail(WalletError::RadioFailure);
    return false;
  }
  auto* control = service->createCharacteristic(
      evm_wallet::CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, sizeof(evm_wallet::BeginCommand));
  auto* data =
      service->createCharacteristic(evm_wallet::DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 244);
  statusCharacteristic = service->createCharacteristic(
      evm_wallet::STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY,
      sizeof(evm_wallet::WalletStatus));
  if (!control || !data || !statusCharacteristic) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    fail(WalletError::RadioFailure);
    return false;
  }
  control->setCallbacks(&controlCallbacks);
  data->setCallbacks(&dataCallbacks);
  publishStatus(false);
  if (!service->start()) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    fail(WalletError::RadioFailure);
    return false;
  }
  auto* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(service->getUUID());
  advertising->setName("X3 EVM Wallet");
  hal_ble_bond::restrictAdvertising(*advertising);
  if (!advertising->start()) {
    NimBLEDevice::deinit(true);
    server = nullptr;
    statusCharacteristic = nullptr;
    fail(WalletError::RadioFailure);
    return false;
  }
  active.store(true, std::memory_order_relaxed);
  walletState.store(WalletState::Advertising, std::memory_order_relaxed);
  publishStatus(false);
  LOG_INF("EVM", "Wallet BLE advertising started");
  return true;
}

void HalBleEvmWallet::end() {
  if (!active.exchange(false, std::memory_order_relaxed) && !NimBLEDevice::isInitialized()) return;
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(true);
  }
  server = nullptr;
  statusCharacteristic = nullptr;
  requestReady.store(false, std::memory_order_relaxed);
  pairingPasskeyValue.store(0, std::memory_order_relaxed);
  walletState.store(WalletState::Stopped, std::memory_order_relaxed);
  LOG_INF("EVM", "Wallet BLE stopped");
}

bool HalBleEvmWallet::takeRequest(SignRequest& destination) {
  if (!requestReady.exchange(false, std::memory_order_acq_rel)) return false;
  taskENTER_CRITICAL(&stagingMux);
  destination = stagingRequest;
  taskEXIT_CRITICAL(&stagingMux);
  return true;
}

void HalBleEvmWallet::approve(const uint32_t requestId, const uint8_t digest[32], const uint8_t signature[65]) {
  currentRequestId.store(requestId, std::memory_order_relaxed);
  std::memcpy(signedDigest, digest, sizeof(signedDigest));
  std::memcpy(signedResult, signature, sizeof(signedResult));
  walletError.store(WalletError::None, std::memory_order_relaxed);
  walletState.store(WalletState::Approved, std::memory_order_relaxed);
  publishStatus();
}

void HalBleEvmWallet::reject(const uint32_t requestId, const WalletError error) {
  currentRequestId.store(requestId, std::memory_order_relaxed);
  walletError.store(error, std::memory_order_relaxed);
  walletState.store(error == WalletError::None ? WalletState::Rejected : WalletState::Error, std::memory_order_relaxed);
  publishStatus();
}

WalletState HalBleEvmWallet::state() const { return walletState.load(std::memory_order_relaxed); }
WalletError HalBleEvmWallet::error() const { return walletError.load(std::memory_order_relaxed); }
uint32_t HalBleEvmWallet::pairingPasskey() const { return pairingPasskeyValue.load(std::memory_order_relaxed); }
bool HalBleEvmWallet::isActive() const { return active.load(std::memory_order_relaxed); }
