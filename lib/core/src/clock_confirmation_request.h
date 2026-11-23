#pragma once

#include <stddef.h>
#include <stdint.h>

#include "clock_confirmation.h"

namespace att {

static const size_t CLOCK_CONFIRM_REQUEST_MAX_BYTES = 128;

enum ClockConfirmRequestDecodeCode : uint8_t {
  CCRD_OK = 0,
  CCRD_INVALID_REQUEST
};

// Accepts exactly request_id, requested_utc_epoch, confirm_backward and confirm_jump.
// request_id is 12 lowercase hexadecimal characters. Unknown/duplicate/missing
// fields, escapes, non-canonical numbers and trailing
// data are rejected. The output remains unchanged on failure.
ClockConfirmRequestDecodeCode decodeClockConfirmRequest(
    const char* json, size_t length, ClockConfirmRequest& out);

}  // namespace att
