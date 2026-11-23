#include "admin_session_fingerprint.h"

#include "sha256.h"

namespace att {

uint64_t adminSessionFingerprint48(const uint8_t token[16]) {
  if (token == nullptr) return 0;
  Sha256 hash;
  uint8_t digest[32] = {};
  hash.update(token, 16);
  hash.finish(digest);
  uint64_t fingerprint = 0;
  for (size_t i = 0; i < 6; ++i)
    fingerprint = (fingerprint << 8) | digest[i];
  volatile uint8_t* bytes = digest;
  for (size_t i = 0; i < sizeof digest; ++i) bytes[i] = 0;
  return fingerprint;
}

}  // namespace att
