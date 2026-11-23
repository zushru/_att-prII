#pragma once

#include <stddef.h>
#include <stdint.h>

#include "roster_store.h"

namespace att {

static const size_t ROSTER_MUTATION_REQUEST_MAX_BYTES = 256;

enum RosterMutationOperation : uint8_t {
  RMO_SET = 1,
  RMO_ACTIVATE,
  RMO_DEACTIVATE,
  RMO_CREATE
};

struct RosterMutationRequest {
  RosterMutationOperation operation;
  uint16_t id;
  char name[ROSTER_NAME_MAX + 1];
  size_t nameLength;
};

// Accepts exactly {operation,id[,name]}. JSON string escapes are decoded to
// UTF-8 before roster validation. Unknown/duplicate/missing fields, invalid
// Unicode, non-canonical integers and trailing data are rejected. Output is
// unchanged on failure.
bool decodeRosterMutationRequest(const char* json, size_t length,
                                 RosterMutationRequest& out);

}  // namespace att
