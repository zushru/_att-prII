#pragma once

#include "admin_recovery_authorization.h"

#include <stddef.h>

namespace att {

// The recovery controller accepts one exact JSON shape. The body is kept
// deliberately small; all large inventories remain server-side.
static const size_t RECOVERY_HTTP_REQUEST_MAX_BYTES = 384;

struct RecoveryHttpRequest {
  RecoveryPreview preview;
  char confirmation[RECOVERY_CONFIRMATION_MAX_BYTES + 1];
};

// Decode a prepare/resume request. The preview is treated only as a challenge:
// the service re-derives it from storage before any mutation. Escapes, unknown
// and duplicate fields, non-canonical numbers/hex, and trailing bytes fail.
bool decodeRecoveryHttpRequest(const char* json, size_t length,
                               RecoveryHttpRequest& out);
void clearRecoveryHttpRequest(RecoveryHttpRequest& request);

}  // namespace att
