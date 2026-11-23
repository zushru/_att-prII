#pragma once

#include <stddef.h>
#include <stdint.h>
#include "attendance_mutation_request.h"
#include "event_group_audit.h"
#include "log_store.h"

namespace att {

enum AttendanceWriteState : uint8_t {
  AWS_COMMITTED,
  AWS_REPLAYED,
  AWS_LOOKUP_PENDING,
  AWS_ID_REUSED,
  AWS_REJECTED,
  AWS_BLOCKED,
  AWS_UNCERTAIN
};
struct AttendanceWriteOutcome {
  AttendanceWriteState state;
  ApiCode code;
  ApiReason reason;
  uint32_t sequence;  // operation event sequence; zero when no commit is confirmed
};

// Durable request-idempotent writer for operator-entered marks, no-class
// changes and the narrow current-day status cycle. Historical edits remain
// three-record groups. Current-day commits store their exact 22-byte request
// fields in three metadata records, avoiding probabilistic hash identity.
// Automatic No-class review similarly stores its full log snapshot across
// three CAS records so request-ID retries remain exact after reboot.
// Legacy two-record identity groups remain replayable, but no new unaudited
// group may be created. Lookup scans 128 ordinary records per invocation and
// completes an in-flight group before yielding. Four audited IDs can make
// independent progress. Legacy no-reason retries use one separate compatibility
// cursor. Inactive cursors expire after 120 monotonic seconds; expiry only
// restarts a durable log scan and can never discard a committed operation.
// A failed append/readback fences LogStore until reopen.
class AttendanceEventWriter {
 public:
  static const size_t LOOKUP_RECORD_BUDGET = 128;
  static const size_t LOOKUP_CONTEXT_COUNT = 4;
  static const uint32_t LOOKUP_CONTEXT_IDLE_TIMEOUT_S = 120;

  AttendanceEventWriter(LogStore& log, EventGroupAudit& audit,
                        LogObserver* const* requiredObservers,
                        size_t observerCount);
  AttendanceEventWriter(const AttendanceEventWriter&) = delete;
  AttendanceEventWriter& operator=(const AttendanceEventWriter&) = delete;

  AttendanceWriteOutcome mark(const MarkReq& request,
                              const AttendanceMutationRequestId& requestId,
                              AdminMutationReason reason,
                              uint64_t sessionFingerprint,
                              const ApiCtx& context, const RosterStore& roster);
  // New generic mark writes are restricted to past school dates. Exact
  // retries of an already-committed request still resolve before this gate.
  // Same-day student changes belong exclusively to the Today cycle endpoint.
  AttendanceWriteOutcome markHistorical(
      const MarkReq& request, const AttendanceMutationRequestId& requestId,
      AdminMutationReason reason, uint64_t sessionFingerprint,
      const ApiCtx& context, const RosterStore& roster);
  // Historical register edits are compare-and-swap writes against the exact
  // log snapshot shown to the operator. Exact retries still replay by request
  // identity even if later unrelated events have been appended.
  AttendanceWriteOutcome markIfSnapshot(
      const MarkReq& request, const AttendanceMutationRequestId& requestId,
      AdminMutationReason reason, uint64_t sessionFingerprint,
      const ApiCtx& context, const RosterStore& roster,
      bool historicalCellEligible, uint32_t expectedRecords,
      uint64_t expectedGeneration);
  AttendanceWriteOutcome todayStatus(
      const TodayStatusMutationRequest& request,
      const AttendanceMutationRequestId& requestId,
      uint64_t sessionFingerprint, const ApiCtx& context,
      RosterStore& roster, bool eligible,
      uint32_t currentRevision, uint8_t effectiveStatus = 0);
  AttendanceWriteOutcome noClass(const NoClassReq& request,
                                 const AttendanceMutationRequestId& requestId,
                                 AdminMutationReason reason,
                                 uint64_t sessionFingerprint,
                                 const ApiCtx& context);
  AttendanceWriteOutcome reviewAutomaticNoClass(
      const AutomaticNoClassReviewRequest& request,
      const AttendanceMutationRequestId& requestId,
      uint64_t sessionFingerprint, const ApiCtx& context,
      bool eligible);
  bool faulted() const { return faulted_; }

 private:
  bool ready_();
  AttendanceWriteOutcome mark_(const MarkReq& request,
      const AttendanceMutationRequestId& requestId, AdminMutationReason reason,
      uint64_t sessionFingerprint, const ApiCtx& context,
      const RosterStore& roster, bool historicalOnly);
  AttendanceWriteOutcome lookup_(const AttendanceMutationRequestId& requestId,
                                 const Event& signature,
                                 AdminMutationReason reason,
                                 uint64_t sessionFingerprint,
                                 uint32_t nowMonotonicS, bool allowBloomMiss,
                                 bool& shouldAppend,
                                 uint64_t& appendSessionFingerprint,
                                 const TodayStatusMutationRequest* todayRequest = nullptr,
                                 const AutomaticNoClassReviewRequest* autoReviewRequest = nullptr);
  AttendanceWriteOutcome appendToday_(const AttendanceMutationRequestId& requestId,
                                 uint64_t sessionFingerprint,
                                 const Event casIdentity[3],
                                 const Event& operation);
  AttendanceWriteOutcome appendAutoNoClassReview_(
      const AttendanceMutationRequestId& requestId,
      uint64_t sessionFingerprint, const Event casIdentity[3],
      const Event& operation);
  AttendanceWriteOutcome append_(const AttendanceMutationRequestId& requestId,
                                 AdminMutationReason reason,
                                 uint64_t sessionFingerprint,
                                 const Event& operation);
  AttendanceWriteOutcome result_(AttendanceWriteState state, ApiCode code,
                                 ApiReason reason, uint32_t sequence = 0) const;
  struct LookupContext {
    bool active;
    AttendanceMutationRequestId requestId;
    Event signature;
    TodayStatusMutationRequest todayRequest;
    bool todayRequestValid;
    AutomaticNoClassReviewRequest autoReviewRequest;
    bool autoReviewRequestValid;
    AdminMutationReason reason;
    uint64_t sessionFingerprint;
    size_t cursor;
    uint32_t lastTouchedMonotonicS;
  };
  void resetLookup_(LookupContext& lookup,
                    const AttendanceMutationRequestId& requestId,
                    const Event& signature, AdminMutationReason reason,
                    uint64_t sessionFingerprint, uint32_t nowMonotonicS,
                    const TodayStatusMutationRequest* todayRequest,
                    const AutomaticNoClassReviewRequest* autoReviewRequest);

  LogStore& log_;
  EventGroupAudit& audit_;
  LogObserver* required_[4];
  size_t requiredCount_;
  uint64_t generation_;
  bool faulted_;
  LookupContext lookups_[LOOKUP_CONTEXT_COUNT];
  LookupContext legacyLookup_;
};

}  // namespace att
