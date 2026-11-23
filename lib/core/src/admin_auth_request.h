#pragma once

#include <stddef.h>
#include <stdint.h>

namespace att {

static const size_t ADMIN_AUTH_REQUEST_MAX_BYTES = 192;

enum AdminAuthRequestKind : uint8_t {
  AAR_LOGIN,
  AAR_LOGOUT,
  AAR_PIN_CHANGE
};

struct AdminAuthRequest {
  uint8_t token[16];
  char pin[7];
  char currentPin[7];
  char newPin[7];
};

// Strict, bounded JSON decoders. Only the exact fields for the selected
// operation are accepted. Duplicate, unknown, escaped and trailing data fail.
// The output is unchanged on failure; caller must clear it after use.
bool decodeAdminAuthRequest(AdminAuthRequestKind kind, const char* json,
                            size_t length, AdminAuthRequest& out);
void clearAdminAuthRequest(AdminAuthRequest& request);

}  // namespace att
