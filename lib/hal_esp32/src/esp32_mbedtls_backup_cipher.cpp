#include "esp32_mbedtls_backup_cipher.h"

// The production adapter is backed by the ESP-IDF mbedTLS build. PlatformIO's
// native test environment does not provide that SDK (nor should it fake AES-GCM).
// Keep the native symbol linkable but deliberately fail closed; tests exercise
// the format through an injected ICipher and separately verify this boundary.
#if defined(ATT_NATIVE_TEST)
namespace att {
namespace {
void secureClear(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}
}  // namespace

bool Esp32MbedTlsBackupCipher::deriveKey(
    const uint8_t* pass, size_t passLength, const uint8_t salt[16],
    uint32_t iterations, uint8_t key[32]) {
  (void)pass;
  (void)passLength;
  (void)salt;
  (void)iterations;
  if (key) secureClear(key, 32);
  return false;
}

bool Esp32MbedTlsBackupCipher::seal(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aadLength,
    const uint8_t* plain, size_t length,
    uint8_t* cipherText, uint8_t tag[16]) {
  (void)key;
  (void)nonce;
  (void)aad;
  (void)aadLength;
  (void)plain;
  if (length && cipherText) secureClear(cipherText, length);
  if (tag) secureClear(tag, 16);
  return false;
}

bool Esp32MbedTlsBackupCipher::open(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aadLength,
    const uint8_t* cipherText, size_t length,
    const uint8_t tag[16], uint8_t* plain) {
  (void)key;
  (void)nonce;
  (void)aad;
  (void)aadLength;
  (void)cipherText;
  (void)tag;
  if (length && plain) secureClear(plain, length);
  return false;
}
}  // namespace att
#else
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <string.h>

namespace att {
namespace {

void secureClear(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

bool validBuffers(const uint8_t* key, const uint8_t* nonce,
                  const uint8_t* aad, size_t aadLength,
                  const uint8_t* input, size_t length,
                  uint8_t* output) {
  return key != nullptr && nonce != nullptr &&
      (aadLength == 0 || aad != nullptr) &&
      (length == 0 || (input != nullptr && output != nullptr));
}

}  // namespace

bool Esp32MbedTlsBackupCipher::deriveKey(
    const uint8_t* pass, size_t passLength, const uint8_t salt[16],
    uint32_t iterations, uint8_t key[32]) {
  if (!key) return false;
  secureClear(key, 32);
  if (!pass || passLength == 0 || !salt ||
      iterations < BK_ITER_MIN || iterations > BK_ITER_MAX)
    return false;

  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, pass, passLength, salt, 16, iterations, 32, key);
  if (rc != 0) {
    secureClear(key, 32);
    return false;
  }
  return true;
}

bool Esp32MbedTlsBackupCipher::seal(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aadLength,
    const uint8_t* plain, size_t length,
    uint8_t* cipherText, uint8_t tag[16]) {
  if (!tag || !validBuffers(key, nonce, aad, aadLength,
                            plain, length, cipherText))
    return false;
  secureClear(tag, 16);

  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
        &context, MBEDTLS_GCM_ENCRYPT, length, nonce, 12,
        aad, aadLength, plain, cipherText, 16, tag);
  }
  mbedtls_gcm_free(&context);
  secureClear(&context, sizeof context);
  if (rc != 0) {
    if (length && cipherText) secureClear(cipherText, length);
    secureClear(tag, 16);
    return false;
  }
  return true;
}

bool Esp32MbedTlsBackupCipher::open(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aadLength,
    const uint8_t* cipherText, size_t length,
    const uint8_t tag[16], uint8_t* plain) {
  if (!tag || !validBuffers(key, nonce, aad, aadLength,
                            cipherText, length, plain))
    return false;
  if (length && plain) secureClear(plain, length);

  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(
        &context, length, nonce, 12, aad, aadLength,
        tag, 16, cipherText, plain);
  }
  mbedtls_gcm_free(&context);
  secureClear(&context, sizeof context);
  if (rc != 0) {
    if (length && plain) secureClear(plain, length);
    return false;
  }
  return true;
}

}  // namespace att
#endif  // ATT_NATIVE_TEST
