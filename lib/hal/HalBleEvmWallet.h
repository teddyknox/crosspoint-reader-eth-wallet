#pragma once

#include <EvmWalletProtocol.h>

#include <cstdint>

class HalBleEvmWallet {
 public:
  static HalBleEvmWallet& getInstance();

  bool begin(const uint8_t address[20]);
  void end();
  bool takeRequest(evm_wallet::SignRequest& destination);
  void approve(uint32_t requestId, const uint8_t digest[32], const uint8_t signature[65]);
  void reject(uint32_t requestId, evm_wallet::WalletError error = evm_wallet::WalletError::None);

  evm_wallet::WalletState state() const;
  evm_wallet::WalletError error() const;
  uint32_t pairingPasskey() const;
  bool isActive() const;

 private:
  HalBleEvmWallet() = default;
};

#define EVM_WALLET_BLE HalBleEvmWallet::getInstance()
