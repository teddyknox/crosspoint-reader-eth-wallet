#include "Keccak256.h"

#include <cstring>

namespace evm_wallet {
namespace {

constexpr size_t RATE = 136;
constexpr uint64_t ROUND_CONSTANTS[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL,
    0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL, 0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

constexpr uint8_t ROTATION[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                  27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr uint8_t PI_LANE[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

uint64_t rotateLeft(const uint64_t value, const uint8_t shift) { return (value << shift) | (value >> (64U - shift)); }

void permute(uint64_t state[25]) {
  for (const uint64_t constant : ROUND_CONSTANTS) {
    uint64_t column[5];
    for (size_t x = 0; x < 5; ++x) {
      column[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
    }
    for (size_t x = 0; x < 5; ++x) {
      const uint64_t delta = column[(x + 4) % 5] ^ rotateLeft(column[(x + 1) % 5], 1);
      for (size_t y = 0; y < 25; y += 5) state[y + x] ^= delta;
    }

    uint64_t current = state[1];
    for (size_t i = 0; i < 24; ++i) {
      const size_t lane = PI_LANE[i];
      const uint64_t next = state[lane];
      state[lane] = rotateLeft(current, ROTATION[i]);
      current = next;
    }

    for (size_t y = 0; y < 25; y += 5) {
      for (size_t x = 0; x < 5; ++x) column[x] = state[y + x];
      for (size_t x = 0; x < 5; ++x) state[y + x] = column[x] ^ ((~column[(x + 1) % 5]) & column[(x + 2) % 5]);
    }
    state[0] ^= constant;
  }
}

void xorByte(uint64_t state[25], const size_t offset, const uint8_t value) {
  state[offset / 8] ^= static_cast<uint64_t>(value) << ((offset % 8) * 8);
}

}  // namespace

Keccak256::Keccak256() = default;

void Keccak256::update(const uint8_t* data, const size_t length) {
  if (finished || (!data && length != 0)) return;
  for (size_t i = 0; i < length; ++i) {
    xorByte(state, position++, data[i]);
    if (position == RATE) {
      permute(state);
      position = 0;
    }
  }
}

void Keccak256::finish(uint8_t output[32]) {
  if (finished || !output) return;
  // Ethereum uses original Keccak padding (0x01), not FIPS SHA3 padding (0x06).
  xorByte(state, position, 0x01);
  xorByte(state, RATE - 1, 0x80);
  permute(state);

  for (size_t i = 0; i < 32; ++i) output[i] = static_cast<uint8_t>(state[i / 8] >> ((i % 8) * 8));
  std::memset(state, 0, sizeof(state));
  position = 0;
  finished = true;
}

void keccak256(const uint8_t* data, const size_t length, uint8_t output[32]) {
  Keccak256 hasher;
  hasher.update(data, length);
  hasher.finish(output);
}

}  // namespace evm_wallet
