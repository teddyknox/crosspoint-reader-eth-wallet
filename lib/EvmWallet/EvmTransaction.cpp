#include "EvmTransaction.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Keccak256.h"

namespace evm_wallet {
namespace {

constexpr uint8_t ERC20_TRANSFER[4] = {0xa9, 0x05, 0x9c, 0xbb};
constexpr uint8_t ERC20_APPROVE[4] = {0x09, 0x5e, 0xa7, 0xb3};
constexpr char HEX[] = "0123456789abcdef";

struct RlpItem {
  const uint8_t* encoded = nullptr;
  const uint8_t* payload = nullptr;
  size_t payloadLength = 0;
  size_t encodedLength = 0;
  bool list = false;
};

bool decodeLength(const uint8_t* input, const size_t length, const size_t lengthOfLength, size_t& result) {
  if (lengthOfLength == 0 || lengthOfLength > 2 || length < lengthOfLength || input[0] == 0) return false;
  result = 0;
  for (size_t i = 0; i < lengthOfLength; ++i) result = (result << 8U) | input[i];
  return true;
}

bool decodeItem(const uint8_t* input, const size_t length, RlpItem& item) {
  if (!input || length == 0) return false;
  const uint8_t prefix = input[0];
  if (prefix <= 0x7f) {
    item = {input, input, 1, 1, false};
    return true;
  }
  if (prefix <= 0xb7) {
    const size_t payloadLength = prefix - 0x80;
    if (length < payloadLength + 1 || (payloadLength == 1 && input[1] <= 0x7f)) return false;
    item = {input, input + 1, payloadLength, payloadLength + 1, false};
    return true;
  }
  if (prefix <= 0xbf) {
    const size_t lengthOfLength = prefix - 0xb7;
    size_t payloadLength = 0;
    if (!decodeLength(input + 1, length - 1, lengthOfLength, payloadLength) || payloadLength < 56 ||
        length < 1 + lengthOfLength + payloadLength)
      return false;
    item = {input, input + 1 + lengthOfLength, payloadLength, 1 + lengthOfLength + payloadLength, false};
    return true;
  }
  if (prefix <= 0xf7) {
    const size_t payloadLength = prefix - 0xc0;
    if (length < payloadLength + 1) return false;
    item = {input, input + 1, payloadLength, payloadLength + 1, true};
    return true;
  }
  const size_t lengthOfLength = prefix - 0xf7;
  size_t payloadLength = 0;
  if (!decodeLength(input + 1, length - 1, lengthOfLength, payloadLength) || payloadLength < 56 ||
      length < 1 + lengthOfLength + payloadLength)
    return false;
  item = {input, input + 1 + lengthOfLength, payloadLength, 1 + lengthOfLength + payloadLength, true};
  return true;
}

bool parseInteger(const RlpItem& item, uint8_t output[32]) {
  if (item.list || item.payloadLength > 32 || (item.payloadLength > 0 && item.payload[0] == 0)) return false;
  std::memset(output, 0, 32);
  if (item.payloadLength > 0) std::memcpy(output + 32 - item.payloadLength, item.payload, item.payloadLength);
  return true;
}

bool toU64(const RlpItem& item, uint64_t& output) {
  if (item.list || item.payloadLength > 8 || (item.payloadLength > 0 && item.payload[0] == 0)) return false;
  output = 0;
  for (size_t i = 0; i < item.payloadLength; ++i) output = (output << 8U) | item.payload[i];
  return true;
}

uint8_t divideByTen(uint8_t value[32]) {
  uint16_t remainder = 0;
  for (size_t i = 0; i < 32; ++i) {
    const uint16_t current = static_cast<uint16_t>((remainder << 8U) | value[i]);
    value[i] = static_cast<uint8_t>(current / 10U);
    remainder = current % 10U;
  }
  return static_cast<uint8_t>(remainder);
}

bool isZero(const uint8_t value[32]) {
  for (size_t i = 0; i < 32; ++i)
    if (value[i] != 0) return false;
  return true;
}

bool parseAccessList(const RlpItem& list, uint16_t& addressCount, uint16_t& storageKeyCount) {
  if (!list.list) return false;
  addressCount = 0;
  storageKeyCount = 0;
  size_t listOffset = 0;
  while (listOffset < list.payloadLength) {
    RlpItem entry;
    if (!decodeItem(list.payload + listOffset, list.payloadLength - listOffset, entry) || !entry.list ||
        addressCount == UINT16_MAX)
      return false;

    size_t entryOffset = 0;
    RlpItem address;
    if (!decodeItem(entry.payload, entry.payloadLength, address) || address.list || address.payloadLength != 20)
      return false;
    entryOffset += address.encodedLength;

    RlpItem storageKeys;
    if (!decodeItem(entry.payload + entryOffset, entry.payloadLength - entryOffset, storageKeys) ||
        !storageKeys.list)
      return false;
    entryOffset += storageKeys.encodedLength;
    if (entryOffset != entry.payloadLength) return false;

    size_t keyOffset = 0;
    while (keyOffset < storageKeys.payloadLength) {
      RlpItem key;
      if (!decodeItem(storageKeys.payload + keyOffset, storageKeys.payloadLength - keyOffset, key) || key.list ||
          key.payloadLength != 32 || storageKeyCount == UINT16_MAX)
        return false;
      ++storageKeyCount;
      keyOffset += key.encodedLength;
    }
    if (keyOffset != storageKeys.payloadLength) return false;

    ++addressCount;
    listOffset += entry.encodedLength;
  }
  return listOffset == list.payloadLength;
}

}  // namespace

ParseError parseEip1559(const uint8_t* transaction, const size_t length, ParsedTransaction& output) {
  output = {};
  if (!transaction || length < 3 || transaction[0] != 0x02) return ParseError::InvalidEnvelope;

  RlpItem envelope;
  if (!decodeItem(transaction + 1, length - 1, envelope) || !envelope.list || envelope.encodedLength != length - 1)
    return ParseError::InvalidRlp;

  RlpItem fields[9];
  size_t offset = 0;
  for (auto& field : fields) {
    if (!decodeItem(envelope.payload + offset, envelope.payloadLength - offset, field)) return ParseError::InvalidRlp;
    offset += field.encodedLength;
  }
  if (offset != envelope.payloadLength) return ParseError::InvalidRlp;

  if (!toU64(fields[0], output.chainId) || output.chainId == 0) return ParseError::InvalidChainId;
  if (!toU64(fields[1], output.nonce) || !parseInteger(fields[2], output.maxPriorityFeePerGas) ||
      !parseInteger(fields[3], output.maxFeePerGas) || !toU64(fields[4], output.gasLimit))
    return ParseError::InvalidInteger;
  if (fields[5].list || fields[5].payloadLength != 20) return ParseError::InvalidRecipient;
  std::memcpy(output.recipient, fields[5].payload, 20);
  if (!parseInteger(fields[6], output.value)) return ParseError::InvalidInteger;
  if (!parseAccessList(fields[8], output.accessListAddressCount, output.accessListStorageKeyCount))
    return ParseError::InvalidAccessList;
  output.hasAccessList = fields[8].payloadLength != 0;
  if (output.hasAccessList) keccak256(fields[8].encoded, fields[8].encodedLength, output.accessListHash);
  if (fields[7].list || fields[7].payloadLength > UINT16_MAX) return ParseError::InvalidRlp;

  if (fields[7].payloadLength == 0) {
    output.kind = TransactionKind::NativeTransfer;
  } else if (fields[7].payloadLength == 68 && isZero(output.value) &&
             (std::memcmp(fields[7].payload, ERC20_TRANSFER, 4) == 0 ||
              std::memcmp(fields[7].payload, ERC20_APPROVE, 4) == 0)) {
    const uint8_t* addressWord = fields[7].payload + 4;
    bool canonicalAddress = true;
    for (size_t i = 0; i < 12; ++i) canonicalAddress &= addressWord[i] == 0;
    if (canonicalAddress) {
      std::memcpy(output.contract, output.recipient, 20);
      std::memcpy(output.recipient, addressWord + 12, 20);
      std::memcpy(output.value, fields[7].payload + 36, 32);
      output.kind = std::memcmp(fields[7].payload, ERC20_TRANSFER, 4) == 0 ? TransactionKind::Erc20Transfer
                                                                           : TransactionKind::Erc20Approval;
    } else {
      output.kind = TransactionKind::ContractCall;
    }
  } else {
    output.kind = TransactionKind::ContractCall;
  }

  if (output.kind == TransactionKind::ContractCall) {
    output.calldataLength = static_cast<uint16_t>(fields[7].payloadLength);
    output.hasSelector = fields[7].payloadLength >= sizeof(output.selector);
    if (output.hasSelector) std::memcpy(output.selector, fields[7].payload, sizeof(output.selector));
    keccak256(fields[7].payload, fields[7].payloadLength, output.calldataHash);
  }

  keccak256(transaction, length, output.digest);
  return ParseError::None;
}

void formatAddress(const uint8_t address[20], char output[43]) {
  output[0] = '0';
  output[1] = 'x';
  for (size_t i = 0; i < 20; ++i) {
    output[2 + i * 2] = HEX[address[i] >> 4U];
    output[3 + i * 2] = HEX[address[i] & 0x0fU];
  }
  output[42] = '\0';
}

void formatShortAddress(const uint8_t address[20], char output[14]) {
  char full[43];
  formatAddress(address, full);
  std::memcpy(output, full, 6);
  std::memcpy(output + 6, "...", 3);
  std::memcpy(output + 9, full + 38, 4);
  output[13] = '\0';
}

bool formatInteger(const uint8_t value[32], char* output, const size_t outputSize) {
  if (!output || outputSize < 2) return false;
  uint8_t working[32];
  std::memcpy(working, value, sizeof(working));
  char reversed[78];
  size_t count = 0;
  do {
    if (count == sizeof(reversed)) return false;
    reversed[count++] = static_cast<char>('0' + divideByTen(working));
  } while (!isZero(working));
  if (count + 1 > outputSize) return false;
  for (size_t i = 0; i < count; ++i) output[i] = reversed[count - i - 1];
  output[count] = '\0';
  return true;
}

bool formatWei(const uint8_t value[32], char* output, const size_t outputSize) {
  char integer[79];
  if (!formatInteger(value, integer, sizeof(integer))) return false;
  const size_t digits = std::strlen(integer);
  if (digits <= 18) {
    const size_t zeros = 18 - digits;
    if (2 + zeros + digits + 1 > outputSize) return false;
    output[0] = '0';
    output[1] = '.';
    std::memset(output + 2, '0', zeros);
    std::memcpy(output + 2 + zeros, integer, digits + 1);
  } else {
    if (digits + 2 > outputSize) return false;
    const size_t whole = digits - 18;
    std::memcpy(output, integer, whole);
    output[whole] = '.';
    std::memcpy(output + whole + 1, integer + whole, 18);
    output[digits + 1] = '\0';
  }
  size_t end = std::strlen(output);
  while (end > 0 && output[end - 1] == '0') output[--end] = '\0';
  if (end > 0 && output[end - 1] == '.') output[--end] = '\0';
  return true;
}

}  // namespace evm_wallet
