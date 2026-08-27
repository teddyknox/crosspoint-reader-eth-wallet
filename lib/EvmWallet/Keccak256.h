#pragma once

#include <cstddef>
#include <cstdint>

namespace evm_wallet {

class Keccak256 {
 public:
  Keccak256();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t output[32]);

 private:
  uint64_t state[25]{};
  size_t position = 0;
  bool finished = false;
};

void keccak256(const uint8_t* data, size_t length, uint8_t output[32]);

}  // namespace evm_wallet
