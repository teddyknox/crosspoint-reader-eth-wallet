#include "EvmSignable.h"

#include <cstdio>
#include <cstring>

#include "EvmTransaction.h"
#include "Keccak256.h"

namespace evm_wallet {
namespace {

constexpr size_t MAX_TYPE_NAME = 32;
constexpr size_t MAX_FIELD_NAME = 32;
constexpr size_t MAX_FIELD_VALUE = 512;
constexpr char SIWE_SUFFIX[] = " wants you to sign in with your Ethereum account:";
constexpr char PERSONAL_PREFIX[] =
    "\x19"
    "Ethereum Signed Message:\n";
constexpr char HEX[] = "0123456789abcdef";

// Parsing is synchronous on the single firmware loop. Static scratch avoids a
// >1 KB stack frame and is wiped after each hash operation.
uint8_t hashPreimage[32 * (MAX_TYPED_FIELDS + 1)]{};
char encodedType[832]{};

class Reader {
 public:
  Reader(const uint8_t* input, const size_t inputLength) : data(input), length(inputLength) {}

  bool readU8(uint8_t& value) {
    if (offset >= length) return false;
    value = data[offset++];
    return true;
  }

  bool readU16(uint16_t& value) {
    if (length - offset < 2) return false;
    value = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8U);
    offset += 2;
    return true;
  }

  bool readU64(uint64_t& value) {
    if (length - offset < 8) return false;
    value = 0;
    for (uint8_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[offset + i]) << (i * 8U);
    offset += 8;
    return true;
  }

  bool readSpan(const size_t count, ByteSpan& span) {
    if (count > UINT16_MAX || length - offset < count) return false;
    span = {data + offset, static_cast<uint16_t>(count)};
    offset += count;
    return true;
  }

  bool atEnd() const { return offset == length; }

 private:
  const uint8_t* data;
  size_t length;
  size_t offset = 0;
};

bool equal(const ByteSpan span, const char* text) {
  const size_t length = std::strlen(text);
  return span.length == length && std::memcmp(span.data, text, length) == 0;
}

bool equalIgnoreCase(const ByteSpan left, const ByteSpan right) {
  if (left.length != right.length) return false;
  for (size_t i = 0; i < left.length; ++i) {
    uint8_t a = left.data[i];
    uint8_t b = right.data[i];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
  }
  return true;
}

bool validIdentifier(const ByteSpan value) {
  if (value.length == 0 || value.length > MAX_FIELD_NAME) return false;
  const auto first = value.data[0];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) return false;
  for (size_t i = 1; i < value.length; ++i) {
    const auto c = value.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool printableAscii(const ByteSpan value) {
  if (value.length == 0) return false;
  for (size_t i = 0; i < value.length; ++i)
    if (value.data[i] < 0x20 || value.data[i] > 0x7e) return false;
  return true;
}

bool parseDecimal(const ByteSpan value, uint64_t& output) {
  if (value.length == 0) return false;
  output = 0;
  for (size_t i = 0; i < value.length; ++i) {
    const uint8_t c = value.data[i];
    if (c < '0' || c > '9' || output > (UINT64_MAX - (c - '0')) / 10U) return false;
    output = output * 10U + (c - '0');
  }
  return true;
}

bool parseHexAddress(const ByteSpan value, uint8_t output[20]) {
  if (value.length != 42 || value.data[0] != '0' || value.data[1] != 'x') return false;
  for (size_t i = 0; i < 20; ++i) {
    uint8_t result = 0;
    for (size_t nibble = 0; nibble < 2; ++nibble) {
      const uint8_t c = value.data[2 + i * 2 + nibble];
      uint8_t digit = 0;
      if (c >= '0' && c <= '9')
        digit = c - '0';
      else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;
      else
        return false;
      result = static_cast<uint8_t>((result << 4U) | digit);
    }
    output[i] = result;
  }
  return true;
}

bool parseType(TypedField& field) {
  if (equal(field.type, "address")) {
    field.kind = TypedValueKind::Address;
    return field.value.length == 20;
  }
  if (equal(field.type, "bool")) {
    field.kind = TypedValueKind::Bool;
    return field.value.length == 1 && field.value.data[0] <= 1;
  }
  if (equal(field.type, "string")) {
    field.kind = TypedValueKind::String;
    return field.value.length <= MAX_FIELD_VALUE;
  }
  if (equal(field.type, "bytes")) {
    field.kind = TypedValueKind::Bytes;
    return field.value.length <= MAX_FIELD_VALUE;
  }
  if (field.type.length >= 5 && std::memcmp(field.type.data, "bytes", 5) == 0) {
    const ByteSpan widthText{field.type.data + 5, static_cast<uint16_t>(field.type.length - 5)};
    uint64_t width = 0;
    if (!parseDecimal(widthText, width) || width == 0 || width > 32 || field.value.length != width) return false;
    field.kind = TypedValueKind::FixedBytes;
    field.bitWidth = static_cast<uint16_t>(width * 8U);
    return true;
  }
  if (field.type.length >= 4 && std::memcmp(field.type.data, "uint", 4) == 0) {
    uint64_t width = 256;
    if (field.type.length > 4) {
      const ByteSpan widthText{field.type.data + 4, static_cast<uint16_t>(field.type.length - 4)};
      if (!parseDecimal(widthText, width)) return false;
    }
    if (width < 8 || width > 256 || width % 8 != 0 || field.value.length > width / 8U ||
        (field.value.length > 1 && field.value.data[0] == 0))
      return false;
    field.kind = TypedValueKind::Uint;
    field.bitWidth = static_cast<uint16_t>(width);
    return true;
  }
  return false;
}

bool readField(Reader& reader, TypedField& field) {
  uint8_t typeLength = 0;
  uint8_t nameLength = 0;
  uint16_t valueLength = 0;
  if (!reader.readU8(typeLength) || typeLength == 0 || typeLength > MAX_TYPE_NAME ||
      !reader.readSpan(typeLength, field.type) || !reader.readU8(nameLength) || nameLength == 0 ||
      nameLength > MAX_FIELD_NAME || !reader.readSpan(nameLength, field.name) || !reader.readU16(valueLength) ||
      valueLength > MAX_FIELD_VALUE || !reader.readSpan(valueLength, field.value))
    return false;
  return validIdentifier(field.name) && parseType(field);
}

bool duplicateNames(const TypedField* fields, const uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    for (uint8_t j = i + 1; j < count; ++j)
      if (equalIgnoreCase(fields[i].name, fields[j].name)) return true;
  }
  return false;
}

bool appendBytes(char* output, size_t& used, const size_t capacity, const uint8_t* value, const size_t length) {
  if (capacity - used <= length) return false;
  std::memcpy(output + used, value, length);
  used += length;
  return true;
}

bool hashStruct(const ByteSpan typeName, const TypedField* fields, const uint8_t count, uint8_t output[32]) {
  size_t used = 0;
  if (!appendBytes(encodedType, used, sizeof(encodedType), typeName.data, typeName.length) ||
      !appendBytes(encodedType, used, sizeof(encodedType), reinterpret_cast<const uint8_t*>("("), 1))
    return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (i != 0 && !appendBytes(encodedType, used, sizeof(encodedType), reinterpret_cast<const uint8_t*>(","), 1))
      return false;
    if (!appendBytes(encodedType, used, sizeof(encodedType), fields[i].type.data, fields[i].type.length) ||
        !appendBytes(encodedType, used, sizeof(encodedType), reinterpret_cast<const uint8_t*>(" "), 1) ||
        !appendBytes(encodedType, used, sizeof(encodedType), fields[i].name.data, fields[i].name.length))
      return false;
  }
  if (!appendBytes(encodedType, used, sizeof(encodedType), reinterpret_cast<const uint8_t*>(")"), 1)) return false;
  keccak256(reinterpret_cast<const uint8_t*>(encodedType), used, hashPreimage);

  for (uint8_t i = 0; i < count; ++i) {
    uint8_t* word = hashPreimage + 32 * (i + 1);
    std::memset(word, 0, 32);
    const auto& field = fields[i];
    switch (field.kind) {
      case TypedValueKind::Address:
        std::memcpy(word + 12, field.value.data, 20);
        break;
      case TypedValueKind::Bool:
        word[31] = field.value.data[0];
        break;
      case TypedValueKind::Uint:
        if (field.value.length > 0) std::memcpy(word + 32 - field.value.length, field.value.data, field.value.length);
        break;
      case TypedValueKind::FixedBytes:
        std::memcpy(word, field.value.data, field.value.length);
        break;
      case TypedValueKind::Bytes:
      case TypedValueKind::String:
        keccak256(field.value.data, field.value.length, word);
        break;
    }
  }
  keccak256(hashPreimage, 32 * (count + 1), output);
  std::memset(hashPreimage, 0, sizeof(hashPreimage));
  std::memset(encodedType, 0, sizeof(encodedType));
  return true;
}

bool readLine(const ByteSpan message, size_t& offset, ByteSpan& line) {
  if (offset > message.length) return false;
  const size_t start = offset;
  while (offset < message.length && message.data[offset] != '\n') {
    if (message.data[offset] == '\r' || message.data[offset] < 0x20 || message.data[offset] > 0x7e) return false;
    ++offset;
  }
  line = {message.data + start, static_cast<uint16_t>(offset - start)};
  if (offset < message.length) ++offset;
  return true;
}

bool valueAfterPrefix(const ByteSpan line, const char* prefix, ByteSpan& value) {
  const size_t prefixLength = std::strlen(prefix);
  if (line.length <= prefixLength || std::memcmp(line.data, prefix, prefixLength) != 0) return false;
  value = {line.data + prefixLength, static_cast<uint16_t>(line.length - prefixLength)};
  return true;
}

ByteSpan originAuthority(const ByteSpan origin) {
  size_t start = 0;
  for (size_t i = 0; i + 2 < origin.length; ++i) {
    if (origin.data[i] == ':' && origin.data[i + 1] == '/' && origin.data[i + 2] == '/') {
      start = i + 3;
      break;
    }
  }
  size_t end = start;
  while (end < origin.length && origin.data[end] != '/' && origin.data[end] != '?' && origin.data[end] != '#') ++end;
  return {origin.data + start, static_cast<uint16_t>(end - start)};
}

bool trustedOriginScheme(const ByteSpan origin) {
  constexpr char https[] = "https://";
  constexpr char localhost[] = "http://localhost";
  constexpr char loopback[] = "http://127.0.0.1";
  return (origin.length > sizeof(https) - 1 && std::memcmp(origin.data, https, sizeof(https) - 1) == 0) ||
         (origin.length >= sizeof(localhost) - 1 && std::memcmp(origin.data, localhost, sizeof(localhost) - 1) == 0) ||
         (origin.length >= sizeof(loopback) - 1 && std::memcmp(origin.data, loopback, sizeof(loopback) - 1) == 0);
}

bool validNonce(const ByteSpan nonce) {
  if (nonce.length < 8) return false;
  for (size_t i = 0; i < nonce.length; ++i) {
    const uint8_t c = nonce.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

}  // namespace

SignableError parseEip712(const uint8_t* payload, const size_t length, ParsedTypedData& output) {
  output = ParsedTypedData{};
  if (!payload || length == 0) return SignableError::InvalidEncoding;
  Reader reader(payload, length);
  uint16_t originLength = 0;
  uint8_t primaryLength = 0;
  if (!reader.readU16(originLength) || originLength == 0 || originLength > 192 ||
      !reader.readSpan(originLength, output.origin) || !printableAscii(output.origin) ||
      !trustedOriginScheme(output.origin) || !reader.readU64(output.chainId) || output.chainId == 0 ||
      !reader.readU8(primaryLength) || primaryLength == 0 || primaryLength > MAX_TYPE_NAME ||
      !reader.readSpan(primaryLength, output.primaryType) || !validIdentifier(output.primaryType))
    return SignableError::InvalidEncoding;

  if (!reader.readU8(output.domainFieldCount) || output.domainFieldCount == 0 || output.domainFieldCount > 5)
    return SignableError::InvalidDomain;
  for (uint8_t i = 0; i < output.domainFieldCount; ++i) {
    if (!readField(reader, output.domainFields[i])) return SignableError::UnsupportedType;
    const auto& field = output.domainFields[i];
    const bool known = equal(field.name, "name") || equal(field.name, "version") || equal(field.name, "chainId") ||
                       equal(field.name, "verifyingContract") || equal(field.name, "salt");
    if (!known) return SignableError::InvalidDomain;
    if (equal(field.name, "chainId")) {
      uint64_t domainChain = 0;
      if (field.value.length > 8) return SignableError::InvalidDomain;
      for (size_t j = 0; j < field.value.length; ++j) domainChain = (domainChain << 8U) | field.value.data[j];
      if (field.kind != TypedValueKind::Uint || domainChain != output.chainId) return SignableError::InvalidDomain;
    } else if (equal(field.name, "verifyingContract")) {
      if (field.kind != TypedValueKind::Address) return SignableError::InvalidDomain;
      std::memcpy(output.verifyingContract, field.value.data, 20);
      output.hasVerifyingContract = true;
    }
  }
  if (duplicateNames(output.domainFields, output.domainFieldCount)) return SignableError::InvalidDomain;

  if (!reader.readU8(output.messageFieldCount) || output.messageFieldCount == 0 ||
      output.messageFieldCount > MAX_TYPED_FIELDS)
    return SignableError::InvalidMessage;
  for (uint8_t i = 0; i < output.messageFieldCount; ++i) {
    if (!readField(reader, output.messageFields[i])) return SignableError::UnsupportedType;
  }
  if (duplicateNames(output.messageFields, output.messageFieldCount) || !reader.atEnd())
    return SignableError::InvalidMessage;

  uint8_t domainHash[32]{};
  uint8_t messageHash[32]{};
  constexpr uint8_t domainType[] = "EIP712Domain";
  const ByteSpan domainTypeSpan{domainType, static_cast<uint16_t>(sizeof(domainType) - 1)};
  if (!hashStruct(domainTypeSpan, output.domainFields, output.domainFieldCount, domainHash) ||
      !hashStruct(output.primaryType, output.messageFields, output.messageFieldCount, messageHash))
    return SignableError::InvalidEncoding;
  const uint8_t prefix[2] = {0x19, 0x01};
  Keccak256 hasher;
  hasher.update(prefix, sizeof(prefix));
  hasher.update(domainHash, sizeof(domainHash));
  hasher.update(messageHash, sizeof(messageHash));
  hasher.finish(output.digest);
  std::memset(domainHash, 0, sizeof(domainHash));
  std::memset(messageHash, 0, sizeof(messageHash));
  return SignableError::None;
}

SignableError parsePersonalMessage(const uint8_t* payload, const size_t length, ParsedPersonalMessage& output) {
  output = ParsedPersonalMessage{};
  if (!payload || length == 0) return SignableError::InvalidEncoding;
  Reader reader(payload, length);
  uint16_t originLength = 0;
  uint16_t messageLength = 0;
  if (!reader.readU16(originLength) || originLength == 0 || originLength > 192 ||
      !reader.readSpan(originLength, output.origin) || !printableAscii(output.origin) ||
      !trustedOriginScheme(output.origin) || !reader.readU64(output.chainId) || output.chainId == 0 ||
      !reader.readU16(messageLength) || messageLength == 0 || !reader.readSpan(messageLength, output.message) ||
      !reader.atEnd())
    return SignableError::InvalidEncoding;

  size_t lineOffset = 0;
  ByteSpan firstLine;
  if (readLine(output.message, lineOffset, firstLine) && firstLine.length > sizeof(SIWE_SUFFIX) - 1) {
    output.looksLikeSiwe = std::memcmp(firstLine.data + firstLine.length - (sizeof(SIWE_SUFFIX) - 1), SIWE_SUFFIX,
                                       sizeof(SIWE_SUFFIX) - 1) == 0;
  }

  char decimalLength[16];
  const int digits =
      std::snprintf(decimalLength, sizeof(decimalLength), "%u", static_cast<unsigned>(output.message.length));
  if (digits <= 0) return SignableError::InvalidMessage;
  Keccak256 hasher;
  hasher.update(reinterpret_cast<const uint8_t*>(PERSONAL_PREFIX), sizeof(PERSONAL_PREFIX) - 1);
  hasher.update(reinterpret_cast<const uint8_t*>(decimalLength), static_cast<size_t>(digits));
  hasher.update(output.message.data, output.message.length);
  hasher.finish(output.digest);
  return SignableError::None;
}

SignableError parseSiwe(const uint8_t* payload, const size_t length, const uint8_t walletAddress[20],
                        ParsedSiwe& output) {
  output = ParsedSiwe{};
  if (!walletAddress) return SignableError::InvalidEncoding;
  ParsedPersonalMessage personal;
  const SignableError personalError = parsePersonalMessage(payload, length, personal);
  if (personalError != SignableError::None) return personalError;
  output.origin = personal.origin;
  const ByteSpan message = personal.message;
  const uint64_t sessionChainId = personal.chainId;

  size_t offset = 0;
  ByteSpan line;
  if (!readLine(message, offset, line) || line.length <= sizeof(SIWE_SUFFIX) - 1 ||
      std::memcmp(line.data + line.length - (sizeof(SIWE_SUFFIX) - 1), SIWE_SUFFIX, sizeof(SIWE_SUFFIX) - 1) != 0)
    return SignableError::InvalidMessage;
  output.domain = {line.data, static_cast<uint16_t>(line.length - (sizeof(SIWE_SUFFIX) - 1))};
  if (!printableAscii(output.domain) || !equalIgnoreCase(output.domain, originAuthority(output.origin)))
    return SignableError::OriginMismatch;

  uint8_t messageAddress[20]{};
  if (!readLine(message, offset, line) || !parseHexAddress(line, messageAddress) ||
      std::memcmp(messageAddress, walletAddress, sizeof(messageAddress)) != 0)
    return SignableError::AddressMismatch;
  if (!readLine(message, offset, line) || line.length != 0) return SignableError::InvalidMessage;
  if (!readLine(message, offset, line)) return SignableError::InvalidMessage;
  if (line.length != 0) {
    output.statement = line;
    if (!readLine(message, offset, line) || line.length != 0) return SignableError::InvalidMessage;
  }
  if (!readLine(message, offset, line) || !valueAfterPrefix(line, "URI: ", output.uri) || !printableAscii(output.uri))
    return SignableError::InvalidMessage;
  ByteSpan value;
  if (!readLine(message, offset, line) || !valueAfterPrefix(line, "Version: ", value) || !equal(value, "1"))
    return SignableError::InvalidMessage;
  if (!readLine(message, offset, line) || !valueAfterPrefix(line, "Chain ID: ", value) ||
      !parseDecimal(value, output.chainId) || output.chainId == 0 || output.chainId != sessionChainId)
    return SignableError::InvalidDomain;
  if (!readLine(message, offset, line) || !valueAfterPrefix(line, "Nonce: ", output.nonce) || !validNonce(output.nonce))
    return SignableError::InvalidMessage;
  if (!readLine(message, offset, line) || !valueAfterPrefix(line, "Issued At: ", output.issuedAt) ||
      output.issuedAt.length < 20 || !printableAscii(output.issuedAt))
    return SignableError::InvalidMessage;

  bool resources = false;
  while (offset < message.length) {
    if (!readLine(message, offset, line) || line.length == 0) return SignableError::InvalidMessage;
    if (resources) {
      if (line.length <= 2 || line.data[0] != '-' || line.data[1] != ' ') return SignableError::InvalidMessage;
      continue;
    }
    if (valueAfterPrefix(line, "Expiration Time: ", value)) {
      if (output.expirationTime.length != 0 || value.length < 20 || !printableAscii(value))
        return SignableError::InvalidMessage;
      output.expirationTime = value;
    } else if (valueAfterPrefix(line, "Not Before: ", value) || valueAfterPrefix(line, "Request ID: ", value)) {
      if (!printableAscii(value)) return SignableError::InvalidMessage;
    } else if (equal(line, "Resources:")) {
      resources = true;
    } else {
      return SignableError::InvalidMessage;
    }
  }

  std::memcpy(output.digest, personal.digest, sizeof(output.digest));
  return SignableError::None;
}

bool copySpan(const ByteSpan span, char* output, const size_t outputSize) {
  if (!output || outputSize == 0 || span.length + 1 > outputSize) return false;
  if (span.length > 0) std::memcpy(output, span.data, span.length);
  output[span.length] = '\0';
  return true;
}

bool formatTypedValue(const TypedField& field, char* output, const size_t outputSize) {
  if (!output || outputSize < 2) return false;
  if (field.kind == TypedValueKind::Address) {
    char address[43];
    formatAddress(field.value.data, address);
    return std::snprintf(output, outputSize, "%.6s...%.4s", address, address + 38) > 0;
  }
  if (field.kind == TypedValueKind::Bool) {
    return std::snprintf(output, outputSize, "%s", field.value.data[0] == 0 ? "false" : "true") > 0;
  }
  if (field.kind == TypedValueKind::Uint) {
    uint8_t integer[32]{};
    if (field.value.length > 0) std::memcpy(integer + 32 - field.value.length, field.value.data, field.value.length);
    return formatInteger(integer, output, outputSize);
  }
  if (field.kind == TypedValueKind::String) {
    const size_t count = field.value.length < outputSize - 1 ? field.value.length : outputSize - 1;
    std::memcpy(output, field.value.data, count);
    output[count] = '\0';
    return true;
  }
  const size_t shown = field.value.length < 8 ? field.value.length : 8;
  if (outputSize < 3 + shown * 2 + (shown < field.value.length ? 3 : 0)) return false;
  output[0] = '0';
  output[1] = 'x';
  for (size_t i = 0; i < shown; ++i) {
    output[2 + i * 2] = HEX[field.value.data[i] >> 4U];
    output[3 + i * 2] = HEX[field.value.data[i] & 0x0fU];
  }
  size_t end = 2 + shown * 2;
  if (shown < field.value.length) {
    std::memcpy(output + end, "...", 3);
    end += 3;
  }
  output[end] = '\0';
  return true;
}

}  // namespace evm_wallet
