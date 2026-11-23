#pragma once

#include "backup_format.h"
#include "log_index.h"
#include "roster_store.h"
#include "event_group_audit.h"

namespace att {

enum RestoreReplayCode : uint8_t {
  RESTORE_REPLAY_OK,
  RESTORE_REPLAY_NOT_VALIDATED,
  RESTORE_REPLAY_STAGE_SIZE_FAILED,
  RESTORE_REPLAY_SIZE_MISMATCH,
  RESTORE_REPLAY_STAGE_READ_FAILED,
  RESTORE_REPLAY_STAGE_CHANGED,
  RESTORE_REPLAY_STORAGE_MISMATCH,
  RESTORE_REPLAY_ATTACH_FAILED,
  RESTORE_REPLAY_LOG_INVALID,
  RESTORE_REPLAY_RECORD_MISMATCH,
  RESTORE_REPLAY_GROUP_MALFORMED,
  RESTORE_REPLAY_ROSTER_MALFORMED,
  RESTORE_REPLAY_ROSTER_OVERFLOW,
  RESTORE_REPLAY_GAP_OVERFLOW,
  RESTORE_REPLAY_CLOSE_OVERFLOW
};

struct RestoreReplayResult {
  RestoreReplayCode code;
  uint32_t records;
  uint32_t lastSeq;
  uint32_t malformedRoster;
  bool rosterOverflow;
  bool gapOverflow;
  bool closeOverflow;
};

typedef EventGroupAudit RestoreGroupAudit;

// Keeps all restore projections alive and preserves their diagnostics even when
// one projection detects malformed input. The one-shot audit classifies each
// projection after replay; this fan-out must not veto the diagnostic pass.
class RestoreReplayObserverFanout final : public LogObserver {
 public:
  RestoreReplayObserverFanout(RosterStore& roster, LogIndex& index,
                              RestoreGroupAudit& groups);
  void onReset() override;
  void onRecord(const Event& event) override;
  bool healthy() const override { return true; }
  bool matchesRecords(uint32_t count, uint32_t crc) const {
    return recordsSeen_ == count && recordsCrc_.value() == crc;
  }
 private:
  RosterStore& roster_;
  LogIndex& index_;
  RestoreGroupAudit& groups_;
  Crc32 recordsCrc_;
  uint32_t recordsSeen_;
};

// Construct only after validator.ok(): this object snapshots the validated Config.
// Replays a successfully authenticated RestoreValidator staging stream through fresh
// production observers. This validates roster semantics and rebuilds a standalone
// LogIndex after the staged LogStore has opened successfully. It does not modify the
// live log and does not perform a storage swap. The object is intentionally one-shot
// because LogStore::open() may reject a damaged staged tail while reporting the
// replay result. The staged record CRC is checked before open and again over the
// exact observer replay, detecting accidental mutation across the audit passes
// (not an adversarial MAC).
//
// Large objects: RosterStore and LogIndex contain fixed-size tables. Allocate this
// coordinator statically or on the heap, never as a local on a small task stack.
class RestoreReplayAudit {
 public:
  RestoreReplayAudit(IStorage& staged, const RestoreValidator& validator);
  ~RestoreReplayAudit();
  RestoreReplayAudit(const RestoreReplayAudit&) = delete;
  RestoreReplayAudit& operator=(const RestoreReplayAudit&) = delete;

  RestoreReplayResult run();
  bool ran() const { return ran_; }
  bool usesValidator(const RestoreValidator& validator) const {
    return &validator_ == &validator;
  }
  const RosterStore& roster() const { return roster_; }
  const LogIndex& index() const { return index_; }

 private:
  RestoreReplayResult finish_(RestoreReplayCode code, const OpenInfo* opened = nullptr);

  IStorage& staged_;
  const RestoreValidator& validator_;
  Config config_;
  LogStore replayLog_;
  RosterStore roster_;
  LogIndex index_;
  RestoreGroupAudit groups_;
  RestoreReplayObserverFanout observers_;
  bool validatedAtConstruction_;
  bool ran_;
  RestoreReplayResult result_;
};

}  // namespace att
