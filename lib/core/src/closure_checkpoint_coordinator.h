#pragma once

#include "closure_checkpoint_store.h"
#include "closure_engine.h"

namespace att {

enum ClosureCoordinatorState : uint8_t {
  CCT_COLD,
  CCT_ACTIVE,
  CCT_PAUSED,
  CCT_FAULTED
};

enum ClosureCoordinatorCode : uint8_t {
  CCC_OK,
  CCC_ALREADY_STARTED,
  CCC_INVALID_TIME,
  CCC_CHECKPOINT_MISSING,
  CCC_CHECKPOINT_INVALID,
  CCC_TIME_ROLLBACK,
  CCC_PAUSED_RESULT,
  CCC_NO_TICK,
  CCC_NOT_ACTIVE,
  CCC_LOG_FAULT,
  CCC_ENGINE_FAULT,
  CCC_PERSIST_FAILED,
  CCC_STEP_PENDING
};

// Serializes checkpoint changes with durable GAP writes and enforces the
// checkpoint-before-CLOSE ordering. All methods belong to the single
// serialized application loop; callers must use service(), not independently
// call tick() and ClosureEngine::step(), for scheduled closure processing.
class ClosureCheckpointCoordinator {
 public:
  ClosureCheckpointCoordinator(ClosureEngine& engine, LogStore& log,
      IClosureCheckpointStore& store, int32_t goLiveDay,
      int16_t utcOffsetMin);

  ClosureCoordinatorCode start(uint32_t nowEpoch);
  ClosureCoordinatorCode healthChanged(uint32_t nowEpoch, bool healthy,
                                        uint16_t cause);
  ClosureCoordinatorCode tick(uint32_t nowEpoch);
  ClosureCoordinatorCode service(uint32_t nowEpoch);

  ClosureCoordinatorState state() const { return state_; }
  bool active() const { return state_ == CCT_ACTIVE; }
  bool faulted() const { return state_ == CCT_FAULTED; }
  uint32_t persistedTick() const { return persistedTick_; }

 private:
  bool validNow_(uint32_t nowEpoch) const;
  ClosureCoordinatorCode fault_(ClosureCoordinatorCode code);
  ClosureCoordinatorCode advanceAfterCoverage_(uint32_t nowEpoch);

  ClosureEngine& engine_;
  LogStore& log_;
  IClosureCheckpointStore& store_;
  int32_t goLiveDay_;
  int16_t utcOffsetMin_;
  uint32_t persistedTick_;
  ClosureCoordinatorState state_;
};

}  // namespace att
