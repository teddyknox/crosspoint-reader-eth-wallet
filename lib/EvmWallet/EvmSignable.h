#pragma once

#include <cstddef>
#include <cstdint>

namespace evm_wallet {

inline constexpr size_t MAX_TYPED_FIELDS = 12;

struct ByteSpan {
  const uint8_t* data = nullptr;
  uint16_t length = 0;
};

enum class TypedValueKind : uint8_t { Address, Bool, Uint, FixedBytes, Bytes, String };

struct TypedField {
  ByteSpan type;
  ByteSpan name;
  ByteSpan value;
  TypedValueKind kind = TypedValueKind::String;
  uint16_t bitWidth = 0;
};

struct ParsedTypedData {
  ByteSpan origin;
  ByteSpan primaryType;
  uint64_t chainId = 0;
  uint8_t verifyingContract[20]{};
  bool hasVerifyingContract = false;
  uint8_t domainFieldCount = 0;
  uint8_t messageFieldCount = 0;
  TypedField domainFields[5]{};
  TypedField messageFields[MAX_TYPED_FIELDS]{};
  uint8_t digest[32]{};
};

struct ParsedSiwe {
  ByteSpan origin;
  ByteSpan domain;
  ByteSpan statement;
  ByteSpan uri;
  ByteSpan nonce;
  ByteSpan issuedAt;
  ByteSpan expirationTime;
  uint64_t chainId = 0;
  uint8_t digest[32]{};
};

struct ParsedPersonalMessage {
  ByteSpan origin;
  ByteSpan message;
  uint64_t chainId = 0;
  bool looksLikeSiwe = false;
  uint8_t digest[32]{};
};

enum class SignableError : uint8_t {
  None = 0,
  InvalidEncoding,
  UnsupportedType,
  InvalidValue,
  InvalidDomain,
  InvalidMessage,
  OriginMismatch,
  AddressMismatch,
};

SignableError parseEip712(const uint8_t* payload, size_t length, ParsedTypedData& output);
SignableError parsePersonalMessage(const uint8_t* payload, size_t length, ParsedPersonalMessage& output);
SignableError parseSiwe(const uint8_t* payload, size_t length, const uint8_t walletAddress[20], ParsedSiwe& output);
bool copySpan(ByteSpan span, char* output, size_t outputSize);
bool formatTypedValue(const TypedField& field, char* output, size_t outputSize);

}  // namespace evm_wallet
