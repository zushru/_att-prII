#include "closure_checkpoint_coordinator.h"

namespace att {

ClosureCheckpointCoordinator::ClosureCheckpointCoordinator(
    ClosureEngine& engine, LogStore& log, IClosureCheckpointStore& store,
    int32_t goLiveDay, int16_t utcOffsetMin)
    : engine_(engine), log_(log), store_(store), goLiveDay_(goLiveDay),
      utcOffsetMin_(utcOffsetMin), persistedTick_(0), state_(CCT_COLD) {
  // Make direct engine.step() calls obey the same durable CLOSE boundary;
  // only this coordinator can acknowledge the NVS-persisted timestamp.
  engine_.checkpointManaged_ = true;
}

bool ClosureCheckpointCoordinator::validNow_(uint32_t nowEpoch) const {
  if (goLiveDay_ < 0 || goLiveDay_ > MAX_SUPPORTED_DAY ||
      utcOffsetMin_ < -720 || utcOffsetMin_ > 840 || nowEpoch < EPOCH_2020)
    return false;
  const int32_t day = dayNumber(nowEpoch, utcOffsetMin_);
  return day >= goLiveDay_ && day <= MAX_SUPPORTED_DAY;
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::fault_(
    ClosureCoordinatorCode code) {
  state_ = CCT_FAULTED;
  return code;
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::advanceAfterCoverage_(
    uint32_t nowEpoch) {
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  if (!engine_.canAdvanceCheckpoint()) {
    state_ = CCT_PAUSED;
    return CCC_PAUSED_RESULT;
  }
  if (nowEpoch < persistedTick_) return fault_(CCC_TIME_ROLLBACK);
  const ClosureCheckpointStoreCode code = store_.advance(nowEpoch);
  if (code != CCS_OK) return fault_(CCC_PERSIST_FAILED);
  persistedTick_ = nowEpoch;
  engine_.acknowledgeCheckpoint_(nowEpoch);
  state_ = CCT_ACTIVE;
  return CCC_OK;
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::start(uint32_t nowEpoch) {
  if (state_ != CCT_COLD) return CCC_ALREADY_STARTED;
  if (!engine_.usesLog(log_) ||
      !engine_.matchesCalendar(goLiveDay_, utcOffsetMin_) ||
      !engine_.ready()) return fault_(CCC_ENGINE_FAULT);
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  if (!validNow_(nowEpoch)) return fault_(CCC_INVALID_TIME);

  ClosureCheckpoint checkpoint = {};
  const ClosureCheckpointStoreCode loadCode = store_.load(checkpoint);
  if (loadCode == CCS_NOT_FOUND) return fault_(CCC_CHECKPOINT_MISSING);
  if (loadCode != CCS_OK) return fault_(CCC_CHECKPOINT_INVALID);
  if (checkpoint.goLiveDay != goLiveDay_ ||
      checkpoint.lastTickEpoch < EPOCH_2020 ||
      !validNow_(checkpoint.lastTickEpoch))
    return fault_(CCC_CHECKPOINT_INVALID);

  persistedTick_ = checkpoint.lastTickEpoch;
  // onBoot appends any clipped outage GAP before it changes lastTick(). The
  // checkpoint write below is deliberately later than that durable append.
  engine_.onBoot(nowEpoch, checkpoint.lastTickEpoch);
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  if (!engine_.ready()) return fault_(CCC_ENGINE_FAULT);
  if (nowEpoch < checkpoint.lastTickEpoch) {
    state_ = CCT_PAUSED;
    return CCC_TIME_ROLLBACK;
  }
  return advanceAfterCoverage_(nowEpoch);
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::healthChanged(
    uint32_t nowEpoch, bool healthy, uint16_t cause) {
  if (state_ == CCT_COLD || state_ == CCT_FAULTED) return CCC_NOT_ACTIVE;
  if (!validNow_(nowEpoch)) return fault_(CCC_INVALID_TIME);

  engine_.onHealth(nowEpoch, healthy, cause);
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  if (!healthy) {
    state_ = CCT_PAUSED;
    return CCC_PAUSED_RESULT;
  }

  // A healthy input is not proof of recovery by itself. In particular, after
  // RTC rollback the clock must move strictly beyond the persisted tick so
  // onHealth() can append a positive-length GAP and clear the engine's bad
  // latch. Keep PAUSED until the engine confirms that transition.
  if (!engine_.canAdvanceCheckpoint()) {
    state_ = CCT_PAUSED;
    return CCC_PAUSED_RESULT;
  }

  // onHealth() records the unhealthy interval, but it does not advance the
  // engine's periodic in-session tick. Keep the durable checkpoint aligned
  // with that last tick; a reboot may conservatively replay an overlapping GAP.
  // Advancing it to now here would claim a tick the engine never acknowledged.
  if (engine_.lastTick() != persistedTick_)
    return fault_(CCC_ENGINE_FAULT);
  state_ = CCT_ACTIVE;
  return CCC_OK;
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::tick(uint32_t nowEpoch) {
  if (state_ != CCT_ACTIVE) return CCC_NOT_ACTIVE;
  if (!validNow_(nowEpoch)) return fault_(CCC_INVALID_TIME);
  const bool accepted = engine_.onTick(nowEpoch);
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  if (!accepted) {
    if (!engine_.canAdvanceCheckpoint()) {
      state_ = CCT_PAUSED;
      return CCC_PAUSED_RESULT;
    }
    return CCC_NO_TICK;
  }
  if (engine_.lastTick() != nowEpoch)
    return fault_(CCC_ENGINE_FAULT);
  return advanceAfterCoverage_(nowEpoch);
}

ClosureCoordinatorCode ClosureCheckpointCoordinator::service(uint32_t nowEpoch) {
  // Advance the durable coverage point before the engine is allowed to append
  // any CLOSE records for sessions ending at/before this observation. If the
  // checkpoint write is ambiguous, tick() faults and no CLOSE is attempted.
  const ClosureCoordinatorCode tickCode = tick(nowEpoch);
  if (tickCode != CCC_OK && tickCode != CCC_NO_TICK) return tickCode;

  if (!engine_.step(nowEpoch)) {
    if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
    if (!engine_.ready()) return fault_(CCC_ENGINE_FAULT);
    // A due session may still be waiting for its end-boundary heartbeat or a
    // bounded closure append may have been refused by an unhealthy roster.
    // Keep the coordinator ACTIVE, but report that no closure progress occurred.
    return CCC_STEP_PENDING;
  }
  if (!log_.readyToAppend()) return fault_(CCC_LOG_FAULT);
  return CCC_OK;
}

}  // namespace att
