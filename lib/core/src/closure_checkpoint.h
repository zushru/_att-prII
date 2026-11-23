#pragma once

#include <stddef.h>
#include <stdint.h>
#include "time_rules.h"

namespace att {

// Versioned, fixed-width representation of the last trusted closure tick.
// The checkpoint is advisory for gap coverage: a stale value is conservative,
// while a value ahead of the RTC is a hard rollback fault.
const size_t CLOSURE_CHECKPOINT_SIZE = 18;
const uint8_t CLOSURE_CHECKPOINT_VERSION = 1;

struct ClosureCheckpoint {
  int32_t goLiveDay;
  uint32_t lastTickEpoch;
};

enum ClosureCheckpointDecodeCode : uint8_t {
  CCP_OK,
  CCP_BAD_ARGUMENT,
  CCP_BAD_MAGIC,
  CCP_BAD_VERSION,
  CCP_BAD_RESERVED,
  CCP_BAD_CRC,
  CCP_BAD_RANGE,
  CCP_WRONG_GO_LIVE
};

// The encoded value is explicit little-endian and independent of compiler
// struct padding/ABI. `out` is unchanged on failure.
bool encodeClosureCheckpoint(int32_t goLiveDay, uint32_t lastTickEpoch,
                             int16_t utcOffsetMin,
                             uint8_t out[CLOSURE_CHECKPOINT_SIZE]);
ClosureCheckpointDecodeCode decodeClosureCheckpoint(
    const uint8_t* bytes, size_t length, int32_t expectedGoLiveDay,
    int16_t utcOffsetMin, ClosureCheckpoint& out);

}  // namespace att
