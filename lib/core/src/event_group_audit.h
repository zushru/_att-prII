#pragma once

#include "log_store.h"

namespace att {

// Validates current event commit-group framing and key attendance/coverage fields.
// Call attach() after a successful LogStore::open(); it replays the current log.
// The observer must outlive the LogStore registration. This is not a cryptographic
// integrity check or a complete semantic validator for future event types.
class EventGroupAudit : public LogObserver {
 public:
  explicit EventGroupAudit(uint32_t firstSeq = 1);
  EventGroupAudit(const EventGroupAudit&) = delete;
  EventGroupAudit& operator=(const EventGroupAudit&) = delete;
  bool attach(LogStore& log);  // attaches once; when log is open, replays the full log
  // `opened` supplies the original open/repair outcome; current sequence/count
  // are compared against the live LogStore so valid later appends stay auditable.
  bool readyFor(const LogStore& log, const OpenInfo& opened) const;
  void onReset() override;
  void onRecord(const Event& event) override;
  void onAttached() override { attached_ = true; }
  bool canAttach() const override { return !attached_; }
  bool healthy() const override {
    return !malformed_ && !sequenceError_ && !pending();
  }
  bool malformed() const { return malformed_; }
  bool pending() const { return count_ != 0; }
  bool sequenceError() const { return sequenceError_; }
  bool attached() const { return attached_; }
  uint32_t recordsSeen() const { return recordsSeen_; }
  uint32_t lastSeq() const { return lastSeq_; }
  uint32_t generation() const { return generation_; }
  // A negative result is definitive; a positive result may be a Bloom-filter
  // collision and must still be checked against the durable log.
  bool mayContainAdminMutation(const uint16_t requestIdWords[4]) const;

  // A well-framed but unfinished clock transaction is not a storage-format
  // fault; it keeps the clock domain untrusted until its exact ID is resumed.
  bool clockRequestPending() const { return clockRequestPending_; }
  bool pendingClockPairCommitted() const { return pendingClockPairCommitted_; }
  uint32_t pendingClockRequestEpoch() const { return pendingClockRequestEpoch_; }
  bool lastClockRequestCompleted() const { return completedClockRequestSeen_; }
  uint32_t completedClockRequestEpoch() const { return completedClockRequestEpoch_; }
  const uint16_t* pendingClockRequestId() const { return pendingClockRequestId_; }
  const uint16_t* completedClockRequestId() const { return completedClockRequestId_; }

 private:
  bool validGroup_() const;
  void rememberAdminMutation_(const Event& marker);
  bool trackClockJournal_();
  uint32_t firstSeq_;
  Event group_[MAX_GROUP];
  size_t count_;
  bool malformed_;
  bool sequenceError_;
  bool attached_;
  uint32_t recordsSeen_;
  uint32_t lastSeq_;
  uint32_t generation_;
  uint8_t mutationBloom_[1024];  // 8192-bit no-false-negative prefilter
  bool clockRequestPending_;
  bool pendingClockPairCommitted_;
  uint16_t pendingClockRequestId_[3];
  uint32_t pendingClockRequestEpoch_;
  uint32_t pendingClockPreviousEpoch_;
  bool pendingClockGapRequired_;
  uint32_t pendingClockGapCursor_;
  uint32_t pendingClockGapTarget_;
  bool completedClockRequestSeen_;
  uint16_t completedClockRequestId_[3];
  uint32_t completedClockRequestEpoch_;
};

}  // namespace att
