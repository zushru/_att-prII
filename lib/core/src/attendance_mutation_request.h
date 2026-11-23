#pragma once

#include <stddef.h>
#include <stdint.h>
#include "api_logic.h"

namespace att {

static const size_t ATTENDANCE_MUTATION_REQUEST_MAX_BYTES = 256;

// LogStore uses firstSeq=1 in the production attendance application. A new
// three-record audited mutation needs sequence values (count+1)..(count+3),
// while LogStore reserves UINT32_MAX as the nextSeq_ sentinel. Keep the
// pre-append and post-append limits distinct: before <= max-4, after <= max-1.
static const uint32_t ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS =
    UINT32_MAX - 4u;
static const uint32_t ATTENDANCE_MUTATION_MAX_LOG_RECORDS =
    UINT32_MAX - 1u;

// Durable 64-bit request identity. The four words are encoded as sixteen
// lowercase hexadecimal characters in the HTTP body and stored with the
// attendance event in one append-only commit group.
struct AttendanceMutationRequestId {
  uint16_t words[4];
};

// Historical writes are conditional on the exact register snapshot presented
// to the operator. The generation and record count together prevent a stale
// browser from overwriting a newer history projection.

// Narrow current-day status contract. The client sends the exact snapshot and
// row revision it displayed; the writer performs an atomic compare-and-set.
struct TodayStatusMutationRequest {
  uint16_t studentId;
  uint16_t day;
  uint8_t session;
  uint8_t status;
  uint32_t expectedRevision;
  uint32_t expectedRecords;
  uint32_t generationHigh;
  uint32_t generationLow;
};

// Explicit admin review of one exact provisional automatic No-class event.
// The declaration sequence is the compare-and-set identity; log generation
// and record count additionally reject stale browser snapshots.
struct AutomaticNoClassReviewRequest {
  uint16_t day;
  uint8_t session;
  uint8_t action;  // ANCR_CONFIRM or ANCR_UNDO
  uint32_t expectedAutoSequence;
  uint32_t expectedRecords;
  uint32_t generationHigh;
  uint32_t generationLow;
};

struct HistoricalMarkMutationRequest {
  MarkReq mark;
  AttendanceMutationRequestId requestId;
  AdminMutationReason reason;
  uint32_t expectedRecords;
  uint32_t generationHigh;
  uint32_t generationLow;
  // Explicit acknowledgement is required when replacing a fingerprint,
  // prior manual mark, or verified absence. Optional only for replaying
  // legacy in-flight requests from the previous dashboard version.
  bool overrideExisting;
};

// Strict, small JSON contracts for the operator-only attendance endpoints.
// Mark status strings are: clear, present, late, excused.
// No-class scope is either day (whole-day override, no session field) or
// session (requires session 0=AM / 1=PM). Unknown/duplicate fields and
// non-canonical integers are rejected. Output remains unchanged on failure.
bool decodeMarkMutationRequest(const char* json, size_t length, MarkReq& out,
                               AttendanceMutationRequestId& requestId,
                               AdminMutationReason* reason = nullptr);
bool decodeTodayStatusMutationRequest(
    const char* json, size_t length, TodayStatusMutationRequest& out,
    AttendanceMutationRequestId& requestId);
bool decodeAutomaticNoClassReviewRequest(
    const char* json, size_t length, AutomaticNoClassReviewRequest& out,
    AttendanceMutationRequestId& requestId);
bool decodeHistoricalMarkMutationRequest(
    const char* json, size_t length, HistoricalMarkMutationRequest& out);
bool decodeNoClassMutationRequest(const char* json, size_t length,
                                  NoClassReq& out,
                                  AttendanceMutationRequestId& requestId,
                                  AdminMutationReason* reason = nullptr);
const char* adminMutationReasonName(AdminMutationReason reason);
bool parseAdminMutationReason(const char* value, AdminMutationReason& out);

}  // namespace att
