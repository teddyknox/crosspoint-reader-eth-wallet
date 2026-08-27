#include "HalBleBondSecurity.h"

#include <Logging.h>
#include <NimBLEDevice.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>

#include <atomic>

namespace {

std::atomic<bool> trustedBondPresent{false};
NimBLEAddress trustedBondAddress;

bool matchesTrustedBond(const NimBLEConnInfo& connection) {
  return connection.getIdAddress() == trustedBondAddress || connection.getAddress() == trustedBondAddress ||
         NimBLEDevice::isBonded(connection.getIdAddress()) || NimBLEDevice::isBonded(connection.getAddress());
}

class BondStoreCallbacks final : public NimBLEDeviceCallbacks {
  int onStoreStatus(struct ble_store_status_event* event, void* argument) override {
    if (trustedBondPresent.load(std::memory_order_relaxed) && event && event->event_code == BLE_STORE_EVENT_FULL) {
      LOG_ERR("BLE", "Rejected pairing because a trusted phone bond already exists");
      return BLE_HS_ESTORE_CAP;
    }
    return NimBLEDeviceCallbacks::onStoreStatus(event, argument);
  }
};

BondStoreCallbacks bondStoreCallbacks;

}  // namespace

namespace hal_ble_bond {

bool initialize() {
  NimBLEDevice::setDeviceCallbacks(&bondStoreCallbacks);
  const int bondCount = NimBLEDevice::getNumBonds();
  if (bondCount > 1) {
    LOG_ERR("BLE", "Refusing to start with %d stored phone bonds", bondCount);
    return false;
  }
  trustedBondPresent.store(bondCount == 1, std::memory_order_relaxed);
  if (bondCount == 0) return true;
  trustedBondAddress = NimBLEDevice::getBondedAddress(0);
  if (!NimBLEDevice::whiteListAdd(trustedBondAddress)) {
    trustedBondPresent.store(false, std::memory_order_relaxed);
    LOG_ERR("BLE", "Could not whitelist the trusted phone bond");
    return false;
  }
  return true;
}

bool authorizeOrAdopt(const NimBLEConnInfo& connection) {
  if (!connection.isEncrypted() || !connection.isAuthenticated() || !connection.isBonded()) return false;
  if (trustedBondPresent.load(std::memory_order_relaxed)) return matchesTrustedBond(connection);
  if (NimBLEDevice::getNumBonds() != 1) return false;
  trustedBondAddress = NimBLEDevice::getBondedAddress(0);
  if (!NimBLEDevice::whiteListAdd(trustedBondAddress)) return false;
  trustedBondPresent.store(true, std::memory_order_relaxed);
  LOG_INF("BLE", "Trusted phone bond saved");
  return true;
}

bool isAuthorized(const NimBLEConnInfo& connection) {
  return trustedBondPresent.load(std::memory_order_relaxed) && connection.isEncrypted() &&
         connection.isAuthenticated() && connection.isBonded() && matchesTrustedBond(connection);
}

bool hasTrustedPhone() { return trustedBondPresent.load(std::memory_order_relaxed); }

void restrictAdvertising(NimBLEAdvertising& advertising) {
  advertising.setScanFilter(false, trustedBondPresent.load(std::memory_order_relaxed));
}

bool forgetTrustedPhone() {
  const bool initializedHere = !NimBLEDevice::isInitialized();
  if (initializedHere && !NimBLEDevice::init("CrossPoint Bluetooth")) {
    LOG_ERR("BLE", "Could not initialize Bluetooth to remove trusted phone bond");
    return false;
  }

  const bool deleted = NimBLEDevice::getNumBonds() == 0 || NimBLEDevice::deleteAllBonds();
  if (deleted) {
    trustedBondPresent.store(false, std::memory_order_relaxed);
    trustedBondAddress = NimBLEAddress();
  }
  if (initializedHere) NimBLEDevice::deinit(true);
  LOG_INF("BLE", "Trusted phone bond removal %s", deleted ? "succeeded" : "failed");
  return deleted;
}

}  // namespace hal_ble_bond
