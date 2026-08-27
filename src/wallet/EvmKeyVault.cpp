#include "EvmKeyVault.h"

#include <Keccak256.h>
#include <Logging.h>
#include <Memory.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <cstring>

namespace {

constexpr char NAMESPACE[] = "x3evm";
constexpr char LEGACY_PRIVATE_KEY[] = "private";
constexpr char ENCRYPTED_VAULT[] = "vault";
constexpr char PUBLIC_ADDRESS[] = "address";
constexpr uint8_t VAULT_VERSION = 1;
// 100,000 rounds took roughly ten seconds on the X3 during the initial
// migration. Keep accepting those records, but use a cost that keeps normal
// unlocks responsive on this hardware. Older records are rewrapped after the
// first successful unlock.
constexpr uint32_t PBKDF2_ITERATIONS = 25000;
constexpr uint32_t MIN_PBKDF2_ITERATIONS = 10000;
constexpr uint32_t MAX_PBKDF2_ITERATIONS = 200000;
constexpr size_t SALT_LENGTH = 16;
constexpr size_t NONCE_LENGTH = 12;
constexpr size_t TAG_LENGTH = 16;
constexpr uint8_t VAULT_AAD[] = {'X', '3', 'E', 'V', 'M', 'V', 'A', 'U', 'L', 'T', VAULT_VERSION};

struct VaultRecord {
  uint8_t version;
  uint8_t iterations[4];
  uint8_t salt[SALT_LENGTH];
  uint8_t nonce[NONCE_LENGTH];
  uint8_t ciphertext[32];
  uint8_t tag[TAG_LENGTH];
};

static_assert(sizeof(VaultRecord) == 81);
constexpr uint8_t SECP256K1_ORDER[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6,
    0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41, 0x00, 0x00, 0x00, 0x01,
};
constexpr uint8_t SECP256K1_HALF_ORDER[32] = {
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x5d, 0x57, 0x6e, 0x73,
    0x57, 0xa4, 0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0, 0x7f, 0xff, 0xff, 0xff,
};

void secureZero(void* memory, const size_t length) {
  volatile auto* bytes = static_cast<volatile uint8_t*>(memory);
  for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

void writeU32(uint8_t output[4], const uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24U);
  output[1] = static_cast<uint8_t>(value >> 16U);
  output[2] = static_cast<uint8_t>(value >> 8U);
  output[3] = static_cast<uint8_t>(value);
}

uint32_t readU32(const uint8_t input[4]) {
  return (static_cast<uint32_t>(input[0]) << 24U) | (static_cast<uint32_t>(input[1]) << 16U) |
         (static_cast<uint32_t>(input[2]) << 8U) | static_cast<uint32_t>(input[3]);
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, const size_t length) {
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

int deriveEncryptionKey(const uint8_t pin[EvmKeyVault::PIN_LENGTH], const uint8_t salt[SALT_LENGTH],
                        const uint32_t iterations, uint8_t output[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pin, EvmKeyVault::PIN_LENGTH, salt, SALT_LENGTH, iterations,
                                       32, output);
}

int encryptPrivateKey(const uint8_t source[32], const uint8_t pin[EvmKeyVault::PIN_LENGTH], VaultRecord& record) {
  record = {};
  record.version = VAULT_VERSION;
  writeU32(record.iterations, PBKDF2_ITERATIONS);
  esp_fill_random(record.salt, sizeof(record.salt));
  esp_fill_random(record.nonce, sizeof(record.nonce));

  uint8_t encryptionKey[32]{};
  int result = deriveEncryptionKey(pin, record.salt, PBKDF2_ITERATIONS, encryptionKey);
  auto context = makeUniqueNoThrow<mbedtls_gcm_context>();
  if (!context) {
    secureZero(encryptionKey, sizeof(encryptionKey));
    LOG_ERR("EVM", "OOM: AES-GCM context");
    return -1;
  }
  mbedtls_gcm_init(context.get());
  if (result == 0) result = mbedtls_gcm_setkey(context.get(), MBEDTLS_CIPHER_ID_AES, encryptionKey, 256);
  if (result == 0) {
    result =
        mbedtls_gcm_crypt_and_tag(context.get(), MBEDTLS_GCM_ENCRYPT, 32, record.nonce, sizeof(record.nonce), VAULT_AAD,
                                  sizeof(VAULT_AAD), source, record.ciphertext, sizeof(record.tag), record.tag);
  }
  mbedtls_gcm_free(context.get());
  secureZero(encryptionKey, sizeof(encryptionKey));
  return result;
}

int decryptPrivateKey(const VaultRecord& record, const uint8_t pin[EvmKeyVault::PIN_LENGTH], uint8_t output[32]) {
  if (record.version != VAULT_VERSION) return -1;
  const uint32_t iterations = readU32(record.iterations);
  if (iterations < MIN_PBKDF2_ITERATIONS || iterations > MAX_PBKDF2_ITERATIONS) return -1;

  uint8_t encryptionKey[32]{};
  int result = deriveEncryptionKey(pin, record.salt, iterations, encryptionKey);
  auto context = makeUniqueNoThrow<mbedtls_gcm_context>();
  if (!context) {
    secureZero(encryptionKey, sizeof(encryptionKey));
    LOG_ERR("EVM", "OOM: AES-GCM context");
    return -1;
  }
  mbedtls_gcm_init(context.get());
  if (result == 0) result = mbedtls_gcm_setkey(context.get(), MBEDTLS_CIPHER_ID_AES, encryptionKey, 256);
  if (result == 0) {
    result = mbedtls_gcm_auth_decrypt(context.get(), sizeof(record.ciphertext), record.nonce, sizeof(record.nonce),
                                      VAULT_AAD, sizeof(VAULT_AAD), record.tag, sizeof(record.tag), record.ciphertext,
                                      output);
  }
  mbedtls_gcm_free(context.get());
  secureZero(encryptionKey, sizeof(encryptionKey));
  if (result != 0) secureZero(output, 32);
  return result;
}

int hardwareRandom(void*, unsigned char* output, const size_t length) {
  esp_fill_random(output, length);
  return 0;
}

int compare256(const uint8_t left[32], const uint8_t right[32]) {
  for (size_t i = 0; i < 32; ++i) {
    if (left[i] < right[i]) return -1;
    if (left[i] > right[i]) return 1;
  }
  return 0;
}

void subtractFromOrder(const uint8_t value[32], uint8_t output[32]) {
  uint16_t borrow = 0;
  for (size_t i = 32; i-- > 0;) {
    const uint16_t subtrahend = static_cast<uint16_t>(value[i]) + borrow;
    if (SECP256K1_ORDER[i] >= subtrahend) {
      output[i] = static_cast<uint8_t>(SECP256K1_ORDER[i] - subtrahend);
      borrow = 0;
    } else {
      output[i] = static_cast<uint8_t>(256U + SECP256K1_ORDER[i] - subtrahend);
      borrow = 1;
    }
  }
}

int loadPrivate(const uint8_t privateKey[32], mbedtls_ecp_group& group, mbedtls_mpi& scalar) {
  int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256K1);
  if (result == 0) result = mbedtls_mpi_read_binary(&scalar, privateKey, 32);
  if (result == 0) result = mbedtls_ecp_check_privkey(&group, &scalar);
  return result;
}

int derivePublic(const uint8_t privateKey[32], uint8_t output[64]) {
  mbedtls_ecp_group group;
  mbedtls_ecp_point publicPoint;
  mbedtls_mpi scalar;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&publicPoint);
  mbedtls_mpi_init(&scalar);

  int result = loadPrivate(privateKey, group, scalar);
  if (result == 0) result = mbedtls_ecp_mul(&group, &publicPoint, &scalar, &group.G, hardwareRandom, nullptr);
  if (result == 0) result = mbedtls_mpi_write_binary(&publicPoint.MBEDTLS_PRIVATE(X), output, 32);
  if (result == 0) result = mbedtls_mpi_write_binary(&publicPoint.MBEDTLS_PRIVATE(Y), output + 32, 32);

  mbedtls_mpi_free(&scalar);
  mbedtls_ecp_point_free(&publicPoint);
  mbedtls_ecp_group_free(&group);
  return result;
}

}  // namespace

EvmKeyVault& EvmKeyVault::getInstance() {
  static EvmKeyVault instance;
  return instance;
}

bool EvmKeyVault::exists() const {
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool found = preferences.getBytesLength(ENCRYPTED_VAULT) == sizeof(VaultRecord) ||
                     preferences.getBytesLength(LEGACY_PRIVATE_KEY) == 32;
  preferences.end();
  return found;
}

bool EvmKeyVault::isEncrypted() const {
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool encrypted = preferences.getBytesLength(ENCRYPTED_VAULT) == sizeof(VaultRecord);
  preferences.end();
  return encrypted;
}

bool EvmKeyVault::loadLegacyPrivateKey(uint8_t output[32]) const {
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool loaded = preferences.getBytes(LEGACY_PRIVATE_KEY, output, 32) == 32;
  preferences.end();
  return loaded;
}

bool EvmKeyVault::encryptAndStore(const uint8_t source[32], const uint8_t pin[PIN_LENGTH]) {
  uint8_t legacyCheck[32]{};
  const bool legacyPresent = loadLegacyPrivateKey(legacyCheck);
  secureZero(legacyCheck, sizeof(legacyCheck));
  VaultRecord record{};
  const int cryptoResult = encryptPrivateKey(source, pin, record);
  if (cryptoResult != 0) {
    secureZero(&record, sizeof(record));
    LOG_ERR("EVM", "Private key encryption failed (crypto=%d)", cryptoResult);
    return false;
  }

  bool written = false;
  Preferences preferences;
  if (preferences.begin(NAMESPACE, false)) {
    written = preferences.putBytes(ENCRYPTED_VAULT, &record, sizeof(record)) == sizeof(record);
    preferences.end();
  }

  uint8_t verifiedKey[32]{};
  bool verified = false;
  record = {};
  if (written && preferences.begin(NAMESPACE, true)) {
    if (preferences.getBytes(ENCRYPTED_VAULT, &record, sizeof(record)) == sizeof(record)) {
      verified = decryptPrivateKey(record, pin, verifiedKey) == 0 && constantTimeEqual(source, verifiedKey, 32);
    }
    preferences.end();
  }
  secureZero(verifiedKey, sizeof(verifiedKey));
  secureZero(&record, sizeof(record));
  if (!verified) {
    LOG_ERR("EVM", "Encrypted private key verification failed");
    return false;
  }

  if (legacyPresent) {
    bool removed = false;
    if (preferences.begin(NAMESPACE, false)) {
      removed = preferences.remove(LEGACY_PRIVATE_KEY);
      preferences.end();
    }
    if (!removed) {
      if (preferences.begin(NAMESPACE, false)) {
        preferences.remove(ENCRYPTED_VAULT);
        preferences.end();
      }
      LOG_ERR("EVM", "Plaintext private key removal failed");
      return false;
    }
  }
  std::memcpy(privateKey, source, sizeof(privateKey));
  unlocked = true;
  LOG_INF("EVM", "Private key encrypted and verified");
  return true;
}

bool EvmKeyVault::createOrEncrypt(const uint8_t pin[PIN_LENGTH]) {
  if (isEncrypted()) return false;

  uint8_t source[32]{};
  if (loadLegacyPrivateKey(source)) {
    const bool migrated = encryptAndStore(source, pin);
    secureZero(source, sizeof(source));
    LOG_INF("EVM", "Legacy wallet migration %s", migrated ? "succeeded" : "failed");
    return migrated;
  }

  mbedtls_ecp_group group;
  mbedtls_mpi scalar;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&scalar);

  int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256K1);
  if (result == 0) result = mbedtls_mpi_random(&scalar, 1, &group.N, hardwareRandom, nullptr);
  if (result == 0) result = mbedtls_mpi_write_binary(&scalar, source, sizeof(source));
  const bool success = result == 0 && encryptAndStore(source, pin);

  mbedtls_mpi_free(&scalar);
  mbedtls_ecp_group_free(&group);
  secureZero(source, sizeof(source));
  LOG_INF("EVM", "Encrypted wallet key generation %s (crypto=%d)", success ? "succeeded" : "failed", result);
  return success;
}

bool EvmKeyVault::unlock(const uint8_t pin[PIN_LENGTH]) {
  lock();
  VaultRecord record{};
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool loaded = preferences.getBytes(ENCRYPTED_VAULT, &record, sizeof(record)) == sizeof(record);
  preferences.end();
  const uint32_t storedIterations = loaded ? readU32(record.iterations) : 0;
  const int result = loaded ? decryptPrivateKey(record, pin, privateKey) : -1;
  if (result != 0) {
    secureZero(&record, sizeof(record));
    lock();
    LOG_INF("EVM", "Wallet unlock failed");
    return false;
  }
  uint8_t legacyKey[32]{};
  if (loadLegacyPrivateKey(legacyKey)) {
    const bool matches = constantTimeEqual(privateKey, legacyKey, sizeof(legacyKey));
    secureZero(legacyKey, sizeof(legacyKey));
    bool removed = false;
    if (matches && preferences.begin(NAMESPACE, false)) {
      removed = preferences.remove(LEGACY_PRIVATE_KEY);
      preferences.end();
    }
    if (!removed) {
      secureZero(&record, sizeof(record));
      lock();
      LOG_ERR("EVM", "Interrupted plaintext migration could not be completed");
      return false;
    }
  }

  if (storedIterations != PBKDF2_ITERATIONS) {
    LOG_INF("EVM", "Rewrapping wallet key with device-appropriate PIN cost");
    if (!encryptAndStore(privateKey, pin)) {
      bool restored = false;
      if (preferences.begin(NAMESPACE, false)) {
        restored = preferences.putBytes(ENCRYPTED_VAULT, &record, sizeof(record)) == sizeof(record);
        preferences.end();
      }
      if (!restored) {
        secureZero(&record, sizeof(record));
        lock();
        LOG_ERR("EVM", "Wallet rewrap and rollback failed");
        return false;
      }
      LOG_ERR("EVM", "Wallet rewrap failed; retained the previous encrypted vault");
    } else {
      LOG_INF("EVM", "Wallet key rewrap succeeded");
    }
  }
  secureZero(&record, sizeof(record));
  unlocked = true;
  LOG_INF("EVM", "Wallet unlocked");
  return true;
}

bool EvmKeyVault::verifyPin(const uint8_t pin[PIN_LENGTH]) const {
  if (!unlocked) return false;

  VaultRecord record{};
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool loaded = preferences.getBytes(ENCRYPTED_VAULT, &record, sizeof(record)) == sizeof(record);
  preferences.end();

  uint8_t candidate[32]{};
  const bool verified = loaded && decryptPrivateKey(record, pin, candidate) == 0 &&
                        constantTimeEqual(candidate, privateKey, sizeof(candidate));
  secureZero(candidate, sizeof(candidate));
  secureZero(&record, sizeof(record));
  LOG_INF("EVM", "Current wallet PIN verification %s", verified ? "succeeded" : "failed");
  return verified;
}

bool EvmKeyVault::changePin(const uint8_t newPin[PIN_LENGTH]) {
  if (!unlocked) return false;

  Preferences preferences;
  VaultRecord previous{};
  if (!preferences.begin(NAMESPACE, true)) return false;
  const bool loaded = preferences.getBytes(ENCRYPTED_VAULT, &previous, sizeof(previous)) == sizeof(previous);
  preferences.end();
  if (!loaded) {
    secureZero(&previous, sizeof(previous));
    return false;
  }

  VaultRecord replacement{};
  const int cryptoResult = encryptPrivateKey(privateKey, newPin, replacement);
  if (cryptoResult != 0) {
    secureZero(&previous, sizeof(previous));
    secureZero(&replacement, sizeof(replacement));
    LOG_ERR("EVM", "PIN change encryption failed (crypto=%d)", cryptoResult);
    return false;
  }

  bool written = false;
  if (preferences.begin(NAMESPACE, false)) {
    written = preferences.putBytes(ENCRYPTED_VAULT, &replacement, sizeof(replacement)) == sizeof(replacement);
    preferences.end();
  }

  VaultRecord persisted{};
  uint8_t verifiedKey[32]{};
  bool verified = false;
  if (written && preferences.begin(NAMESPACE, true)) {
    if (preferences.getBytes(ENCRYPTED_VAULT, &persisted, sizeof(persisted)) == sizeof(persisted)) {
      verified = decryptPrivateKey(persisted, newPin, verifiedKey) == 0 &&
                 constantTimeEqual(privateKey, verifiedKey, sizeof(verifiedKey));
    }
    preferences.end();
  }
  secureZero(verifiedKey, sizeof(verifiedKey));
  secureZero(&persisted, sizeof(persisted));

  if (!verified) {
    bool restored = false;
    if (preferences.begin(NAMESPACE, false)) {
      restored = preferences.putBytes(ENCRYPTED_VAULT, &previous, sizeof(previous)) == sizeof(previous);
      preferences.end();
    }
    if (restored && preferences.begin(NAMESPACE, true)) {
      VaultRecord restoredRecord{};
      restored =
          preferences.getBytes(ENCRYPTED_VAULT, &restoredRecord, sizeof(restoredRecord)) == sizeof(restoredRecord) &&
          constantTimeEqual(reinterpret_cast<const uint8_t*>(&restoredRecord),
                            reinterpret_cast<const uint8_t*>(&previous), sizeof(previous));
      secureZero(&restoredRecord, sizeof(restoredRecord));
      preferences.end();
    }
    secureZero(&previous, sizeof(previous));
    secureZero(&replacement, sizeof(replacement));
    if (!restored) {
      lock();
      LOG_ERR("EVM", "PIN change and encrypted vault rollback failed");
    } else {
      LOG_ERR("EVM", "PIN change failed; retained the previous encrypted vault");
    }
    return false;
  }

  secureZero(&previous, sizeof(previous));
  secureZero(&replacement, sizeof(replacement));
  LOG_INF("EVM", "Wallet PIN changed and encrypted vault verified");
  return true;
}

void EvmKeyVault::lock() {
  secureZero(privateKey, sizeof(privateKey));
  unlocked = false;
}

bool EvmKeyVault::isUnlocked() const { return unlocked; }

bool EvmKeyVault::address(uint8_t output[20]) const {
  if (!unlocked) {
    Preferences preferences;
    bool loaded = false;
    if (preferences.begin(NAMESPACE, true)) {
      loaded = preferences.getBytes(PUBLIC_ADDRESS, output, 20) == 20;
      preferences.end();
    }
    if (!loaded) std::memset(output, 0, 20);
    LOG_INF("EVM", "Cached wallet address load %s", loaded ? "succeeded" : "failed");
    return loaded;
  }

  uint8_t publicKey[64]{};
  uint8_t digest[32]{};
  int result = -1;
  if (unlocked) result = derivePublic(privateKey, publicKey);
  if (result == 0) {
    evm_wallet::keccak256(publicKey, sizeof(publicKey), digest);
    std::memcpy(output, digest + 12, 20);
    uint8_t cached[20]{};
    Preferences preferences;
    bool hasCached = false;
    if (preferences.begin(NAMESPACE, true)) {
      hasCached = preferences.getBytes(PUBLIC_ADDRESS, cached, sizeof(cached)) == sizeof(cached);
      preferences.end();
    }
    if (!hasCached || !constantTimeEqual(output, cached, sizeof(cached))) {
      bool stored = false;
      if (preferences.begin(NAMESPACE, false)) {
        stored = preferences.putBytes(PUBLIC_ADDRESS, output, 20) == 20;
        preferences.end();
      }
      if (!stored) LOG_ERR("EVM", "Wallet address cache write failed");
    }
    secureZero(cached, sizeof(cached));
  } else {
    std::memset(output, 0, 20);
  }
  secureZero(publicKey, sizeof(publicKey));
  secureZero(digest, sizeof(digest));
  LOG_INF("EVM", "Wallet address derivation %s (crypto=%d)", result == 0 ? "succeeded" : "failed", result);
  return result == 0;
}

bool EvmKeyVault::signDigest(const uint8_t digest[32], uint8_t signature[65]) const {
  if (!unlocked) return false;

  mbedtls_ecp_group group;
  mbedtls_ecp_point noncePoint;
  mbedtls_mpi privateScalar;
  mbedtls_mpi nonce;
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_mpi message;
  mbedtls_mpi temporary;
  mbedtls_mpi inverse;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&noncePoint);
  mbedtls_mpi_init(&privateScalar);
  mbedtls_mpi_init(&nonce);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  mbedtls_mpi_init(&message);
  mbedtls_mpi_init(&temporary);
  mbedtls_mpi_init(&inverse);

  int result = loadPrivate(privateKey, group, privateScalar);
  if (result == 0) result = mbedtls_mpi_read_binary(&message, digest, 32);
  bool signedSuccessfully = false;

  for (uint8_t attempt = 0; result == 0 && !signedSuccessfully && attempt < 8; ++attempt) {
    result = mbedtls_mpi_random(&nonce, 1, &group.N, hardwareRandom, nullptr);
    if (result == 0) result = mbedtls_ecp_mul(&group, &noncePoint, &nonce, &group.G, hardwareRandom, nullptr);
    if (result == 0) result = mbedtls_mpi_mod_mpi(&r, &noncePoint.MBEDTLS_PRIVATE(X), &group.N);
    if (result == 0 &&
        (mbedtls_mpi_cmp_int(&r, 0) == 0 || mbedtls_mpi_cmp_mpi(&noncePoint.MBEDTLS_PRIVATE(X), &group.N) >= 0)) {
      continue;
    }
    if (result == 0) result = mbedtls_mpi_mul_mpi(&temporary, &r, &privateScalar);
    if (result == 0) result = mbedtls_mpi_add_mpi(&temporary, &temporary, &message);
    if (result == 0) result = mbedtls_mpi_mod_mpi(&temporary, &temporary, &group.N);
    if (result == 0) result = mbedtls_mpi_inv_mod(&inverse, &nonce, &group.N);
    if (result == 0) result = mbedtls_mpi_mul_mpi(&s, &inverse, &temporary);
    if (result == 0) result = mbedtls_mpi_mod_mpi(&s, &s, &group.N);
    if (result == 0 && mbedtls_mpi_cmp_int(&s, 0) == 0) continue;
    if (result == 0) result = mbedtls_mpi_write_binary(&r, signature, 32);
    if (result == 0) result = mbedtls_mpi_write_binary(&s, signature + 32, 32);
    if (result == 0) {
      uint8_t parity = static_cast<uint8_t>(mbedtls_mpi_get_bit(&noncePoint.MBEDTLS_PRIVATE(Y), 0));
      if (compare256(signature + 32, SECP256K1_HALF_ORDER) > 0) {
        uint8_t lowS[32];
        subtractFromOrder(signature + 32, lowS);
        std::memcpy(signature + 32, lowS, sizeof(lowS));
        secureZero(lowS, sizeof(lowS));
        parity ^= 1U;
      }
      signature[64] = parity;
      signedSuccessfully = true;
    }
  }

  mbedtls_mpi_free(&inverse);
  mbedtls_mpi_free(&temporary);
  mbedtls_mpi_free(&message);
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&nonce);
  mbedtls_mpi_free(&privateScalar);
  mbedtls_ecp_point_free(&noncePoint);
  mbedtls_ecp_group_free(&group);
  if (!signedSuccessfully) std::memset(signature, 0, 65);
  LOG_INF("EVM", "Digest signing %s (crypto=%d)", signedSuccessfully ? "succeeded" : "failed", result);
  return signedSuccessfully;
}
