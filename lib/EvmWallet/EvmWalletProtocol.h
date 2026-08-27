#pragma once

#include <cstddef>
#include <cstdint>

namespace evm_wallet {

inline constexpr uint8_t PROTOCOL_VERSION = 2;
inline constexpr size_t MAX_SIGN_PAYLOAD = 2048;
inline constexpr char REQUEST_MAGIC[4] = {'X', '3', 'E', 'S'};

inline constexpr char SERVICE_UUID[] = "38178710-0a1b-4f29-9803-7f6a6d75de10";
inline constexpr char CONTROL_UUID[] = "38178711-0a1b-4f29-9803-7f6a6d75de10";
inline constexpr char DATA_UUID[] = "38178712-0a1b-4f29-9803-7f6a6d75de10";
inline constexpr char STATUS_UUID[] = "38178713-0a1b-4f29-9803-7f6a6d75de10";

enum class TransactionKind : uint8_t {
  NativeTransfer = 0,
  Erc20Transfer = 1,
  Erc20Approval = 2,
  ContractCall = 3,
};
enum class SignRequestKind : uint8_t {
  Eip1559Transaction = 0,
  Eip712TypedData = 1,
  PersonalMessage = 2,
  EthSignMessage = 3,
};
enum class ControlOpcode : uint8_t { Begin = 1, Commit = 2, Cancel = 3 };
enum class WalletState : uint8_t {
  Stopped = 0,
  Advertising = 1,
  Connected = 2,
  Pairing = 3,
  Receiving = 4,
  ReviewReady = 5,
  Approved = 6,
  Rejected = 7,
  Error = 8,
};
enum class WalletError : uint8_t {
  None = 0,
  InvalidCommand = 1,
  InvalidLength = 2,
  UnexpectedOffset = 3,
  InvalidRequest = 4,
  UnsupportedTransaction = 5,
  SigningFailed = 6,
  RadioFailure = 7,
};

#pragma pack(push, 1)
struct SignRequest {
  char magic[4];
  uint8_t version;
  uint8_t kind;
  uint16_t wireSize;
  uint32_t requestId;
  uint16_t payloadLength;
  uint16_t reserved;
  uint8_t payload[MAX_SIGN_PAYLOAD];
  uint32_t crc32;
};

struct BeginCommand {
  uint8_t opcode;
  uint16_t wireSize;
};

struct WalletStatus {
  uint8_t protocolVersion;
  uint8_t state;
  uint8_t error;
  uint8_t reserved;
  uint16_t receivedBytes;
  uint16_t expectedBytes;
  uint32_t requestId;
  uint8_t address[20];
  uint8_t digest[32];
  uint8_t signature[65];  // r[32] || s[32] || yParity
};
#pragma pack(pop)

static_assert(sizeof(SignRequest) == 2068);
static_assert(sizeof(BeginCommand) == 3);
static_assert(sizeof(WalletStatus) == 129);

uint32_t crc32(const uint8_t* data, size_t length);
uint32_t requestCrc32(const SignRequest& request);
bool validateRequest(const SignRequest& request);
uint8_t batchPosition(const SignRequest& request);
uint8_t batchCount(const SignRequest& request);

}  // namespace evm_wallet
