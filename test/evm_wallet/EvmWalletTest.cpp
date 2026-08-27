#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "EvmSignable.h"
#include "EvmTransaction.h"
#include "EvmWalletProtocol.h"
#include "Keccak256.h"

namespace {

std::vector<uint8_t> fromHex(const char* text) {
  std::vector<uint8_t> result;
  for (size_t i = 0; text[i] && text[i + 1]; i += 2) {
    result.push_back(static_cast<uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16)));
  }
  return result;
}

std::string hex(const uint8_t* bytes, const size_t count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(count * 2, '0');
  for (size_t i = 0; i < count; ++i) {
    result[i * 2] = digits[bytes[i] >> 4U];
    result[i * 2 + 1] = digits[bytes[i] & 0x0fU];
  }
  return result;
}

void appendU16(std::vector<uint8_t>& output, const size_t value) {
  output.push_back(static_cast<uint8_t>(value));
  output.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendU64(std::vector<uint8_t>& output, const uint64_t value) {
  for (uint8_t shift = 0; shift < 64; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}

void appendText(std::vector<uint8_t>& output, const char* value, const bool wide = false) {
  const size_t length = std::strlen(value);
  if (wide)
    appendU16(output, length);
  else
    output.push_back(static_cast<uint8_t>(length));
  output.insert(output.end(), value, value + length);
}

void appendField(std::vector<uint8_t>& output, const char* type, const char* name, const std::vector<uint8_t>& value) {
  appendText(output, type);
  appendText(output, name);
  appendU16(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

TEST(Keccak256, MatchesEthereumVectors) {
  uint8_t digest[32];
  evm_wallet::keccak256(nullptr, 0, digest);
  EXPECT_EQ(hex(digest, 32), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  const uint8_t abc[] = {'a', 'b', 'c'};
  evm_wallet::keccak256(abc, sizeof(abc), digest);
  EXPECT_EQ(hex(digest, 32), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

TEST(EvmWalletProtocol, ValidatesFixedRequestAndCrc) {
  evm_wallet::SignRequest request{};
  std::memcpy(request.magic, evm_wallet::REQUEST_MAGIC, 4);
  request.version = evm_wallet::PROTOCOL_VERSION;
  request.wireSize = sizeof(request);
  request.requestId = 7;
  request.payloadLength = 1;
  request.payload[0] = 0x02;
  request.crc32 = evm_wallet::requestCrc32(request);
  EXPECT_TRUE(evm_wallet::validateRequest(request));
  request.payload[0] ^= 1;
  EXPECT_FALSE(evm_wallet::validateRequest(request));
}

TEST(EvmWalletProtocol, AcceptsEthSignAndValidBatchMetadata) {
  evm_wallet::SignRequest request{};
  std::memcpy(request.magic, evm_wallet::REQUEST_MAGIC, 4);
  request.version = evm_wallet::PROTOCOL_VERSION;
  request.wireSize = sizeof(request);
  request.kind = static_cast<uint8_t>(evm_wallet::SignRequestKind::EthSignMessage);
  request.requestId = 8;
  request.payloadLength = 1;
  request.payload[0] = 0x01;
  request.crc32 = evm_wallet::requestCrc32(request);
  EXPECT_TRUE(evm_wallet::validateRequest(request));

  request.kind = static_cast<uint8_t>(evm_wallet::SignRequestKind::Eip1559Transaction);
  request.reserved = 0x0201;
  request.crc32 = evm_wallet::requestCrc32(request);
  EXPECT_TRUE(evm_wallet::validateRequest(request));
  EXPECT_EQ(evm_wallet::batchPosition(request), 1U);
  EXPECT_EQ(evm_wallet::batchCount(request), 2U);
}

TEST(EvmWalletProtocol, RejectsInvalidBatchMetadata) {
  evm_wallet::SignRequest request{};
  std::memcpy(request.magic, evm_wallet::REQUEST_MAGIC, 4);
  request.version = evm_wallet::PROTOCOL_VERSION;
  request.wireSize = sizeof(request);
  request.requestId = 9;
  request.payloadLength = 1;
  request.payload[0] = 0x01;

  request.kind = static_cast<uint8_t>(evm_wallet::SignRequestKind::Eip1559Transaction);
  request.reserved = 0x0102;
  request.crc32 = evm_wallet::requestCrc32(request);
  EXPECT_FALSE(evm_wallet::validateRequest(request));

  request.kind = static_cast<uint8_t>(evm_wallet::SignRequestKind::PersonalMessage);
  request.reserved = 0x0201;
  request.crc32 = evm_wallet::requestCrc32(request);
  EXPECT_FALSE(evm_wallet::validateRequest(request));
}

TEST(EvmTransaction, ParsesCanonicalNativeEip1559) {
  // 0.01 ETH to 0x1111... on chain 10, with an empty access list.
  const auto tx =
      fromHex("02ee0a01843b9aca008477359400825208941111111111111111111111111111111111111111872386f26fc1000080c0");
  evm_wallet::ParsedTransaction parsed;
  ASSERT_EQ(evm_wallet::parseEip1559(tx.data(), tx.size(), parsed), evm_wallet::ParseError::None);
  EXPECT_EQ(parsed.kind, evm_wallet::TransactionKind::NativeTransfer);
  EXPECT_EQ(parsed.chainId, 10U);
  EXPECT_EQ(parsed.nonce, 1U);
  EXPECT_EQ(parsed.gasLimit, 21000U);
  char amount[96];
  ASSERT_TRUE(evm_wallet::formatWei(parsed.value, amount, sizeof(amount)));
  EXPECT_STREQ(amount, "0.01");
}

TEST(EvmTransaction, ParsesUnknownCalldataAsContractCall) {
  const auto tx = fromHex("02e90a01843b9aca00847735940082520894111111111111111111111111111111111111111180820102c0");
  evm_wallet::ParsedTransaction parsed;
  ASSERT_EQ(evm_wallet::parseEip1559(tx.data(), tx.size(), parsed), evm_wallet::ParseError::None);
  EXPECT_EQ(parsed.kind, evm_wallet::TransactionKind::ContractCall);
  EXPECT_EQ(parsed.calldataLength, 2U);
  EXPECT_FALSE(parsed.hasSelector);
  EXPECT_EQ(hex(parsed.calldataHash, 32), "22ae6da6b482f9b1b19b0b897c3fd43884180a1c5ee361e1107a1bc635649dda");
}

TEST(EvmTransaction, DisplaysSelectorAndHashForArbitraryContractCall) {
  const auto tx =
      fromHex("02ef0a01843b9aca008477359400825208941111111111111111111111111111111111111111808812345678deadbeefc0");
  evm_wallet::ParsedTransaction parsed;
  ASSERT_EQ(evm_wallet::parseEip1559(tx.data(), tx.size(), parsed), evm_wallet::ParseError::None);
  EXPECT_EQ(parsed.kind, evm_wallet::TransactionKind::ContractCall);
  EXPECT_EQ(parsed.calldataLength, 8U);
  EXPECT_TRUE(parsed.hasSelector);
  EXPECT_EQ(hex(parsed.selector, 4), "12345678");
  EXPECT_EQ(hex(parsed.calldataHash, 32), "a97318d55e40cb3096b2050c870088b2377c883c2fa84233dcf62178803a8970");
}

TEST(EvmTransaction, ParsesAndSummarizesTypeTwoAccessList) {
  auto tx = fromHex(
      "02f8670a01843b9aca008477359400825208941111111111111111111111111111111111111111872386f26fc1000080"
      "f838f7942222222222222222222222222222222222222222e1a0333333333333333333333333333333333333333333"
      "3333333333333333333333");
  ASSERT_EQ(tx.size(), 106U);
  evm_wallet::ParsedTransaction parsed;
  ASSERT_EQ(evm_wallet::parseEip1559(tx.data(), tx.size(), parsed), evm_wallet::ParseError::None);
  EXPECT_TRUE(parsed.hasAccessList);
  EXPECT_EQ(parsed.accessListAddressCount, 1U);
  EXPECT_EQ(parsed.accessListStorageKeyCount, 1U);
  EXPECT_EQ(hex(parsed.accessListHash, 32), "93f8c3e34bc296d2679e73c7d740e1d9bd332e533595efc1251806b807822dd9");

  ASSERT_EQ(tx[tx.size() - 33], 0xa0);
  tx[tx.size() - 33] = 0x9f;
  EXPECT_EQ(evm_wallet::parseEip1559(tx.data(), tx.size(), parsed), evm_wallet::ParseError::InvalidAccessList);
}

TEST(EvmTransaction, FormatsFullWidthInteger) {
  uint8_t value[32]{};
  value[31] = 1;
  char output[96];
  ASSERT_TRUE(evm_wallet::formatInteger(value, output, sizeof(output)));
  EXPECT_STREQ(output, "1");
  std::memset(value, 0xff, sizeof(value));
  ASSERT_TRUE(evm_wallet::formatInteger(value, output, sizeof(output)));
  EXPECT_STREQ(output, "115792089237316195423570985008687907853269984665640564039457584007913129639935");
}

TEST(EvmSignable, HashesFlatPermitTypedDataOnDevice) {
  std::vector<uint8_t> payload;
  appendText(payload, "https://example.com", true);
  appendU64(payload, 1);
  appendText(payload, "Permit");
  payload.push_back(4);
  appendField(payload, "string", "name", {'U', 'S', 'D', ' ', 'C', 'o', 'i', 'n'});
  appendField(payload, "string", "version", {'2'});
  appendField(payload, "uint256", "chainId", {1});
  std::vector<uint8_t> contract(20, 0);
  contract[19] = 1;
  appendField(payload, "address", "verifyingContract", contract);
  payload.push_back(5);
  appendField(payload, "address", "owner", std::vector<uint8_t>(20, 0x11));
  appendField(payload, "address", "spender", std::vector<uint8_t>(20, 0x22));
  appendField(payload, "uint256", "value", {0x0f, 0x42, 0x40});
  appendField(payload, "uint256", "nonce", {7});
  appendField(payload, "uint256", "deadline", {0x02, 0x54, 0x0b, 0xe3, 0xff});

  evm_wallet::ParsedTypedData parsed;
  ASSERT_EQ(evm_wallet::parseEip712(payload.data(), payload.size(), parsed), evm_wallet::SignableError::None);
  EXPECT_EQ(parsed.chainId, 1U);
  EXPECT_EQ(hex(parsed.digest, 32), "190f63b9336ffde65049ce607dded049cec3e5699056293ae1a1b7202686ebee");
}

TEST(EvmSignable, ParsesStrictSiweAndHashesOriginalMessage) {
  constexpr char origin[] = "https://example.com";
  constexpr char message[] =
      "example.com wants you to sign in with your Ethereum account:\n"
      "0x1111111111111111111111111111111111111111\n\n"
      "Sign in to the app.\n\n"
      "URI: https://example.com/login\n"
      "Version: 1\n"
      "Chain ID: 1\n"
      "Nonce: abcdef12\n"
      "Issued At: 2026-08-26T12:00:00Z";
  std::vector<uint8_t> payload;
  appendText(payload, origin, true);
  appendU64(payload, 1);
  appendText(payload, message, true);
  uint8_t address[20];
  std::memset(address, 0x11, sizeof(address));
  evm_wallet::ParsedSiwe parsed;
  ASSERT_EQ(evm_wallet::parseSiwe(payload.data(), payload.size(), address, parsed), evm_wallet::SignableError::None);
  EXPECT_EQ(parsed.chainId, 1U);
  EXPECT_EQ(hex(parsed.digest, 32), "14ff756bff03f3e4f188abf77121fe91072673933252f14a92585691eceb18fa");

  payload[10] = 'x';
  EXPECT_EQ(evm_wallet::parseSiwe(payload.data(), payload.size(), address, parsed),
            evm_wallet::SignableError::OriginMismatch);
}

TEST(EvmSignable, HashesGenericPersonalMessageOnDevice) {
  constexpr char origin[] = "https://react-app.walletconnect.com";
  constexpr char message[] = "Hello from WalletConnect";
  std::vector<uint8_t> payload;
  appendText(payload, origin, true);
  appendU64(payload, 11155111);
  appendText(payload, message, true);

  evm_wallet::ParsedPersonalMessage parsed;
  ASSERT_EQ(evm_wallet::parsePersonalMessage(payload.data(), payload.size(), parsed), evm_wallet::SignableError::None);
  EXPECT_FALSE(parsed.looksLikeSiwe);
  EXPECT_EQ(parsed.chainId, 11155111U);
  EXPECT_EQ(hex(parsed.digest, 32), "7de75ca63c9460876848f775b0a94d4d2670666577a1558b1eaaa9a19de67ed8");
}

}  // namespace
