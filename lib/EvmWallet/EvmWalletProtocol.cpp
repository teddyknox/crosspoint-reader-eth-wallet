#include "EvmWalletProtocol.h"

#include <cstddef>
#include <cstring>

namespace evm_wallet {

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

uint32_t requestCrc32(const SignRequest& request) {
  return crc32(reinterpret_cast<const uint8_t*>(&request), offsetof(SignRequest, crc32));
}

bool validateRequest(const SignRequest& request) {
  return std::memcmp(request.magic, REQUEST_MAGIC, sizeof(request.magic)) == 0 && request.version == PROTOCOL_VERSION &&
         request.kind <= static_cast<uint8_t>(SignRequestKind::PersonalMessage) && request.reserved == 0 &&
         request.wireSize == sizeof(SignRequest) && request.payloadLength > 0 &&
         request.payloadLength <= MAX_SIGN_PAYLOAD && request.crc32 == requestCrc32(request);
}

}  // namespace evm_wallet
