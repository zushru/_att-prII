#pragma once

#include "backup_format.h"

namespace att {

// Production ICipher adapter using ESP-IDF's mbedTLS implementation:
// PBKDF2-HMAC-SHA256 and AES-256-GCM. The iteration count is supplied by the
// caller; this class does not invent KDF policy or select a backup password.
class Esp32MbedTlsBackupCipher final : public ICipher {
 public:
  bool deriveKey(const uint8_t* pass, size_t passLength,
                 const uint8_t salt[16], uint32_t iterations,
                 uint8_t key[32]) override;
  bool seal(const uint8_t key[32], const uint8_t nonce[12],
            const uint8_t* aad, size_t aadLength,
            const uint8_t* plain, size_t length,
            uint8_t* cipherText, uint8_t tag[16]) override;
  bool open(const uint8_t key[32], const uint8_t nonce[12],
            const uint8_t* aad, size_t aadLength,
            const uint8_t* cipherText, size_t length,
            const uint8_t tag[16], uint8_t* plain) override;
};

}  // namespace att
