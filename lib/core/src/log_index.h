#pragma once
#include "log_store.h"
#include "time_rules.h"
namespace att {
const size_t MAX_GAPS = 512;     // active-window disjoint coverage spans; overflow = unverified coverage
const size_t MAX_CLOSES = 800;   // 400 active local days x 2 sessions
const size_t MAX_AUTO_NOCLASS_PENDING = 800;  // unresolved alerts survive rolling-window pruning

// Bounded, replay-derived evidence for one (school day, AM/PM) session.
// A session reference is created by any raw scan/manual attendance/no-class
// event or by EV_CLOSE. Overflow is fail-closed for automatic No-class.
// Keep the durable-alert projection compact: supported day numbers fit 15
// bits and the per-session expected count fits 9 bits (MAX_ID is 300).
// Together these fields occupy exactly 8 bytes on the ESP32/GCC ABI.
struct PendingAutomaticNoClass {
  uint32_t declarationSequence;
  uint16_t day : 15;
  uint16_t session : 1;
  uint16_t expectedCount : 9;
  uint16_t reserved : 7;
};

// Bit-pack the six lifecycle flags. The active 400-day window contains at most
// 800 session entries; keeping each entry at 16 bytes avoids a large static-RAM
// increase in the ESP32 closure engine.
struct SessionEvidence {
  int32_t day;
  uint32_t autoDeclarationSequence;
  uint16_t expectedCount;
  uint16_t acceptedScans;
  uint16_t manualAttendanceEntries;
  uint8_t session;
  bool closed : 1;
  bool coverageComplete : 1;
  bool manualNoClass : 1;
  bool autoEvaluated : 1;
  bool autoPending : 1;
  bool autoConfirmed : 1;
};
static_assert(MAX_SUPPORTED_DAY < (1 << 15),
              "pending alert day must fit its 15-bit field");
static_assert(sizeof(PendingAutomaticNoClass) == 8,
              "pending alert entries must remain 8 bytes");
static_assert(sizeof(SessionEvidence) <= 16,
              "session evidence entries must remain compact");

class LogIndex : public LogObserver {
 public:
  LogIndex();
  void onReset() override;
  void onRecord(const Event& e) override;
  // Set before LogStore::attach() so obsolete CLOSE/GAP facts are filtered
  // during the initial raw-log replay, not after bounded capacity is consumed.
  bool setWindow(int32_t firstDay, int32_t lastDay, int16_t utcOffsetMin);
  bool closed(int32_t day, uint8_t session) const;
  bool gapOverlaps(uint32_t ws, uint32_t we) const;   // [ws, we) against merged active coverage spans
  bool gapOverflow() const { return gapOverflow_; }
  bool closeOverflow() const { return closeOverflow_; }
  bool evidenceOverflow() const { return evidenceOverflow_; }
  bool evidenceHealthy() const {
    return !evidenceOverflow_ && !evidenceMalformed_ &&
        pendingAutoNoClassHealthy();
  }
  bool pendingAutoNoClassHealthy() const {
    return !pendingAutoOverflow_ && !pendingAutoMalformed_ && !pendingAutoReview_;
  }
  size_t gapCount() const { return nGaps_; }
  size_t closeCount() const { return nCloses_; }
  size_t evidenceCount() const { return nEvidence_; }
  bool hasWindow() const { return windowSet_; }
  bool sessionEvidence(int32_t day, uint8_t session, SessionEvidence& out) const;
  bool sessionEvidenceAt(size_t index, SessionEvidence& out) const;
  size_t pendingAutoNoClassCount() const { return nPendingAutoNoClass_; }
  bool pendingAutoNoClassAt(size_t index, PendingAutomaticNoClass& out) const;
  // Returns the oldest eligible, not-yet-evaluated zero-evidence closed session.
  // The caller must still verify timetable/clock/health policy before committing.
  bool nextAutoNoClassCandidate(SessionEvidence& out) const;
 private:
  void reset_();
  void clearProjection_();
  void rememberGapOverflow_(uint64_t from, uint64_t to);
  void insertGap_(uint32_t from, uint32_t to);
  size_t findEvidence_(int32_t day, uint8_t session) const;
  size_t findPendingAutoNoClass_(int32_t day, uint8_t session) const;
  bool addPendingAutoNoClass_(int32_t day, uint8_t session,
                              uint32_t sequence, uint16_t expectedCount);
  bool removePendingAutoNoClass_(int32_t day, uint8_t session,
                                 uint32_t sequence);
  size_t ensureEvidence_(int32_t day, uint8_t session);
  void incrementSaturating_(uint16_t& value);
  struct Span { uint32_t from, to; };
  Span gaps_[MAX_GAPS];
  SessionEvidence evidence_[MAX_CLOSES];
  PendingAutomaticNoClass pendingAutoNoClass_[MAX_AUTO_NOCLASS_PENDING];
  size_t nPendingAutoNoClass_;
  size_t nGaps_, nCloses_, nEvidence_;
  bool gapOverflow_, closeOverflow_, evidenceOverflow_, evidenceMalformed_;
  bool pendingAutoOverflow_, pendingAutoMalformed_;
  bool windowSet_;
  int32_t firstDay_, lastDay_;
  int16_t utcOffsetMin_;
  uint64_t windowFrom_, windowTo_;   // [local midnight(first), local midnight(last+1))
  uint64_t overflowFrom_, overflowTo_;
  // The first of three EV_TODAY_CAS records identifies the exact automatic
  // declaration. The next two records preserve snapshot count/generation.
  bool pendingAutoReview_;
  int32_t pendingAutoReviewDay_;
  uint8_t pendingAutoReviewSession_;
  uint32_t pendingAutoReviewTarget_;
};
}
