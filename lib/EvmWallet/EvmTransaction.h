#pragma once

#include <cstddef>
#include <cstdint>

#include "EvmWalletProtocol.h"

namespace evm_wallet {

enum class ParseError : uint8_t {
  None = 0,
  InvalidEnvelope,
  InvalidRlp,
  InvalidChainId,
  InvalidRecipient,
  InvalidAccessList,
  InvalidInteger,
};

struct ParsedTransaction {
  TransactionKind kind = TransactionKind::NativeTransfer;
  uint64_t chainId = 0;
  uint64_t nonce = 0;
  uint64_t gasLimit = 0;
  uint8_t maxPriorityFeePerGas[32]{};
  uint8_t maxFeePerGas[32]{};
  uint8_t recipient[20]{};
  uint8_t value[32]{};
  uint8_t contract[20]{};
  uint16_t calldataLength = 0;
  uint8_t selector[4]{};
  bool hasSelector = false;
  uint8_t calldataHash[32]{};
  bool hasAccessList = false;
  uint16_t accessListAddressCount = 0;
  uint16_t accessListStorageKeyCount = 0;
  uint8_t accessListHash[32]{};
  uint8_t digest[32]{};
};

ParseError parseEip1559(const uint8_t* transaction, size_t length, ParsedTransaction& output);
void formatAddress(const uint8_t address[20], char output[43]);
void formatShortAddress(const uint8_t address[20], char output[14]);
bool formatWei(const uint8_t value[32], char* output, size_t outputSize);
bool formatInteger(const uint8_t value[32], char* output, size_t outputSize);

}  // namespace evm_wallet
