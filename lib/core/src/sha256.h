#pragma once
#include <stddef.h>
#include <stdint.h>

namespace att {

// Small allocation-free SHA-256 used to bind recovery archives to source bytes.
class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t digest[32]);

 private:
  void transform_(const uint8_t block[64]);
  uint32_t state_[8];
  uint64_t totalBytes_;
  uint8_t block_[64];
  size_t used_;
  bool finished_;
};

}  // namespace att
