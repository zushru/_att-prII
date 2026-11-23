#pragma once

#include <stdint.h>

namespace att {

// Returns the first 48 bits of SHA-256(token). This is a short-lived session
// pseudonym, not a user identity; the shared ADMIN PIN does not identify a
// named operator. The caller must erase the raw token after authorization.
uint64_t adminSessionFingerprint48(const uint8_t token[16]);

}  // namespace att
