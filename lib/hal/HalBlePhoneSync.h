#pragma once

#include <PhoneSyncProtocol.h>

#include <cstdint>

class HalBlePhoneSync {
 public:
  static HalBlePhoneSync& getInstance();

  bool begin();
  void end();
  bool takeSnapshot(phone_sync::CalendarSnapshot& destination);
  void acknowledge(uint32_t sequence, bool accepted, phone_sync::SyncError error = phone_sync::SyncError::None);

  phone_sync::SyncState state() const;
  phone_sync::SyncError error() const;
  uint32_t pairingPasskey() const;
  bool isActive() const;

 private:
  HalBlePhoneSync() = default;
};

#define PHONE_SYNC_BLE HalBlePhoneSync::getInstance()
