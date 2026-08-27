#pragma once

#include <cstdint>

class EvmKeyVault {
 public:
  static constexpr uint8_t PIN_LENGTH = 6;

  static EvmKeyVault& getInstance();

  bool exists() const;
  bool isEncrypted() const;
  bool createOrEncrypt(const uint8_t pin[PIN_LENGTH]);
  bool unlock(const uint8_t pin[PIN_LENGTH]);
  bool verifyPin(const uint8_t pin[PIN_LENGTH]) const;
  bool changePin(const uint8_t newPin[PIN_LENGTH]);
  void lock();
  bool isUnlocked() const;
  bool address(uint8_t output[20]) const;
  bool signDigest(const uint8_t digest[32], uint8_t signature[65]) const;

 private:
  EvmKeyVault() = default;

  uint8_t privateKey[32]{};
  bool unlocked = false;

  bool loadLegacyPrivateKey(uint8_t output[32]) const;
  bool encryptAndStore(const uint8_t source[32], const uint8_t pin[PIN_LENGTH]);
};

#define EVM_KEY_VAULT EvmKeyVault::getInstance()
