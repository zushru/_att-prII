#pragma once
#include "day_view.h"
#include "log_index.h"
namespace att {
struct Config;
const uint32_t TICK_PERIOD_S = 300;
// Bounded loop jitter. Longer intervals are explicitly recorded as GAP before
// the next tick is acknowledged; never silently bridge a missed heartbeat.
const uint32_t MAX_TICK_INTERVAL_S = TICK_PERIOD_S + 60u;
const int MAX_TRACK_DAYS = 400;
const int MAX_CLOSURE_DAYS_PER_STEP = 1;
class IRoster {
 public:
  virtual ~IRoster() {}
  virtual bool expected(uint16_t id, int32_t day) = 0;
  // False when the roster projection is incomplete or otherwise untrusted.
  // Closure must not snapshot a partial roster into an apparently valid CLOSE.
  virtual bool historyComplete() const = 0;
};
// NOTE: ClosureEngine embeds a bounded LogIndex (~23 KB with the current
// compact evidence/alert records). Allocate it statically or on the heap,
// never on a task stack; the index is intentionally not stack-local.
class ClosureCheckpointCoordinator;

class ClosureEngine {
 public:
  ClosureEngine(LogStore& log, const Params& p, const Timetable& t, IRoster& r,
                uint16_t cap, int32_t goLiveDay, int32_t initialTodayDay);
  // Timetable updates never alter the immutable provisioning anchor.
  // Call only after config validation/durable commit, serialized with engine work.
  void setSchedule(const Params& p, const Timetable& t);
  void onBoot(uint32_t now, uint32_t lastTickFromNvs);
  void onHealth(uint32_t now, bool healthy, uint16_t cause);
  bool onTick(uint32_t now);
  uint32_t lastTick() const { return lastTick_; }
  bool step(uint32_t now);
  bool pending() const { return pending_; }
  // True only while the engine can safely acknowledge a trusted clock tick.
  bool canAdvanceCheckpoint() const {
    return !bad_ && indexOk_ && index_.evidenceHealthy() &&
        log_.readyToAppend();
  }
  bool ready() const { return indexOk_ && index_.evidenceHealthy(); }
  bool usesLog(const LogStore& log) const { return &log_ == &log; }
  bool matchesCalendar(int32_t goLiveDay, int16_t utcOffsetMin) const {
    return goLiveDay_ == goLiveDay && p_.utcOffsetMin == utcOffsetMin;
  }
  bool usesRoster(const IRoster& roster) const { return &roster_ == &roster; }
  bool matchesConfig(const Config& config) const;
  void invalidate() {}          // no-op: the index follows the log through the observer
  const LogIndex& index() const { return index_; }
 private:
  friend class ClosureCheckpointCoordinator;
  void acknowledgeCheckpoint_(uint32_t epoch) {
    checkpointManaged_ = true;
    checkpointAcknowledgedTick_ = epoch;
  }
  bool writeGap(uint32_t from, uint32_t to, uint16_t cause);
  bool clipToWindows(uint32_t from, uint32_t to, uint32_t& cf, uint32_t& ct) const;
  bool closeSession(int32_t day, int s, uint32_t now);
  bool observeTime_(uint32_t now);
  LogStore& log_; Params p_; Timetable t_; IRoster& roster_;
  uint16_t cap_; int32_t goLiveDay_;
  uint32_t lastTick_, lastObservedNow_, clockRollbackSince_, badSince_, nextDueCheck_;
  int32_t processedThroughDay_;
  uint16_t badCause_;
  uint8_t nextSessionToProcess_;
  bool bad_, pending_, indexOk_, progressReady_, clockRollback_;
  bool checkpointManaged_;
  uint32_t checkpointAcknowledgedTick_;
  LogIndex index_;
};
}
