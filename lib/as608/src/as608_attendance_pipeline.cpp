#include "as608_attendance_pipeline.h"
#include "as608_runtime.h"
#include "enrolled_roster.h"
#include "event_codec.h"
#include "time_rules.h"

namespace att {
namespace {
AttendancePipelineResult logReadinessFailure(LogStore& log) {
  return log.faulted() ? ATT_PIPE_REJECTED : ATT_PIPE_NOT_SAVED;
}
}  // namespace

As608AttendancePipeline::As608AttendancePipeline(As608Runtime& runtime,
    LogStore& log, RosterStore& roster, const Config& config,
    const IAs608PageResolver& pages, IAttendanceAdmission& admission)
    : As608AttendancePipeline(runtime.attendanceClient(), log, roster,
                              runtime.occupancy(), config, pages, admission) {}

As608AttendancePipeline::As608AttendancePipeline(As608AttendanceClient& sensor,
    LogStore& log, RosterStore& roster, const Occupancy& occupancy,
    const Config& config, const IAs608PageResolver& pages,
    IAttendanceAdmission& admission)
    : sensor_(sensor), log_(log), roster_(roster), occupancy_(occupancy),
      config_(config), pages_(pages), admission_(admission), scanEpoch_(0),
      lastStudentId_(0), active_(false), faulted_(false) {}

bool As608AttendancePipeline::canSetConfigAfterCommit(const Config& config) const {
  // RosterStore replays roster-event epochs into effective local days using its
  // construction-time offset. There is no safe live offset-change/replay API yet.
  // Refuse before the A/B slot is written; otherwise roster expectations and the
  // pipeline/closure timetable would use different local-day boundaries.
  return config.goLiveDay == config_.goLiveDay &&
      config.params.utcOffsetMin == roster_.utcOffsetMin() &&
      !faulted_ && !active_ && !sensor_.active() && validateConfig(config) &&
      admission_.canSetParamsAfterCommit(config.params);
}

bool As608AttendancePipeline::setConfigAfterCommit(const Config& config) {
  if (!canSetConfigAfterCommit(config) ||
      !admission_.setParamsAfterCommit(config.params)) return false;
  config_ = config;
  return true;
}

bool As608AttendancePipeline::begin(uint32_t nowEpoch) {
  if (faulted_ || active_ || !sensor_.ready() || !occupancy_.valid ||
      !roster_.historyComplete() || !log_.readyToAppend() || nowEpoch == 0 ||
      log_.lastSeq() == UINT32_MAX) return false;
  const int32_t day = dayNumber(nowEpoch, config_.params.utcOffsetMin);
  if (day < 0 || day > MAX_SUPPORTED_DAY) return false;
  const int session = sessionAt(config_.tt, config_.params, nowEpoch);
  if (session < 0 || session > 1) return false;
  if (!admission_.allowScan(day, (uint8_t)session)) {
    admission_.service();
    if (!admission_.allowScan(day, (uint8_t)session)) return false;
  }
  // Admission is an external policy boundary; revalidate mutable dependencies
  // before reserving the sensor and starting a transaction.
  if (!occupancy_.valid || !roster_.historyComplete() || !log_.readyToAppend())
    return false;
  scanEpoch_ = nowEpoch;
  lastStudentId_ = 0;
  if (!sensor_.beginScanFromPipeline()) return false;
  active_ = true;
  return true;
}

AttendancePipelineResult As608AttendancePipeline::saveMatch_(uint16_t physicalPage,
                                                               uint16_t score) {
  // Occupancy may be invalidated by a future application owner between scan start
  // and commit. Never append an event from a scan spanning an untrusted index state.
  if (!occupancy_.valid || !roster_.historyComplete()) return ATT_PIPE_REJECTED;
  if (!log_.readyToAppend()) return logReadinessFailure(log_);
  uint16_t id = 0;
  if (!pages_.studentIdForPage(physicalPage, id) || id < 1 || id > MAX_ID ||
      !roster_.known(id) || !roster_.active(id))
    return ATT_PIPE_NOT_ACTIVE;

  // Search and index are two independent sensor claims. A page returned by
  // search that is absent from the complete startup index is inconsistent;
  // never let the resolver alone authorize a durable attendance event.
  if (!occupancy_.has(id)) return ATT_PIPE_NOT_ACTIVE;

  lastStudentId_ = id;
  const bool expected = roster_.expected(
      id, dayNumber(scanEpoch_, config_.params.utcOffsetMin));
  // expected() may discover a damaged per-student interval chain and latch the
  // roster unhealthy. Do not collapse that system fault into an ordinary
  // "not active" result or let callers treat it as a normal attendance outcome.
  if (!roster_.historyComplete()) return ATT_PIPE_REJECTED;
  if (!expected) return ATT_PIPE_NOT_ACTIVE;

  const int session = sessionAt(config_.tt, config_.params, scanEpoch_);
  if (session < 0 || session > 1) return ATT_PIPE_REJECTED;
  const int32_t day = dayNumber(scanEpoch_, config_.params.utcOffsetMin);
  if (day < 0 || day > MAX_SUPPORTED_DAY ||
      !admission_.allowScan(day, (uint8_t)session)) return ATT_PIPE_REJECTED;
  if (!occupancy_.valid || !roster_.historyComplete()) return ATT_PIPE_REJECTED;
  if (!log_.readyToAppend()) return logReadinessFailure(log_);

  const bool alreadyScanned = admission_.alreadyScanned(day, (uint8_t)session, id);
  // A policy lookup may have observed a new fault or changed storage readiness.
  // Recheck before either returning a normal duplicate result or attempting commit.
  if (!occupancy_.valid || !roster_.historyComplete()) return ATT_PIPE_REJECTED;
  if (!log_.readyToAppend()) return logReadinessFailure(log_);
  if (alreadyScanned) return ATT_PIPE_ALREADY_RECORDED;

  const uint16_t minute = minuteOfDay(scanEpoch_, config_.params.utcOffsetMin);
  const Slot& slot = config_.tt.day[weekdayIndex(day)][session];
  const uint8_t status = isLate(slot, config_.params.lateMin, minute)
      ? ST_LATE : ST_PRESENT;
  Event event = { EV_SCAN, makeFlags((uint8_t)session, status, false, false, false),
                  id, scanEpoch_, 0, score };
  // A failed append is indeterminate: a complete event may already be durable.
  // Likewise, a successful append is not enough to report success unless the exact
  // committed record can be read back from the authoritative store. In either case
  // fence this pipeline instance; callers must reopen/replay before retrying.
  const size_t beforeCount = log_.count();
  const uint32_t beforeSeq = log_.lastSeq();
  const uint64_t beforeGeneration = log_.openGeneration();
  if (beforeSeq == UINT32_MAX || !log_.append(&event, 1)) {
    faulted_ = true;
    log_.requireReopen();
    return ATT_PIPE_COMMIT_UNCERTAIN;
  }

  Event persisted = {};
  const bool storeVerifiedBytes = log_.appendVerifiesExactBytes();
  const bool exactReadback = beforeCount != SIZE_MAX &&
      log_.count() == beforeCount + 1u &&
      log_.lastSeq() == beforeSeq + 1u &&
      log_.openGeneration() == beforeGeneration && !log_.faulted() &&
      (storeVerifiedBytes ||
       (log_.at(beforeCount, persisted) &&
        persisted.type == event.type && persisted.flags == event.flags &&
        persisted.a == event.a && persisted.epoch == event.epoch &&
        persisted.seq == beforeSeq + 1u && persisted.b == event.b));
  if (!exactReadback) {
    faulted_ = true;
    log_.requireReopen();
    return ATT_PIPE_COMMIT_UNCERTAIN;
  }
  lastStudentId_ = id;
  return status == ST_LATE ? ATT_PIPE_SAVED_LATE : ATT_PIPE_SAVED_PRESENT;
}

AttendancePipelineResult As608AttendancePipeline::step() {
  if (faulted_) return ATT_PIPE_COMMIT_UNCERTAIN;
  if (!active_) return ATT_PIPE_IDLE;
  uint16_t page = 0xFFFF, score = 0;
  const As608AttendanceResult result = sensor_.stepScanFromPipeline(page, score);
  if (result == AS608_ATT_PENDING) return ATT_PIPE_PENDING;
  active_ = false;
  switch (result) {
    case AS608_ATT_MATCH: return saveMatch_(page, score);
    case AS608_ATT_NO_FINGER: return ATT_PIPE_NO_FINGER;
    case AS608_ATT_NO_MATCH: return ATT_PIPE_NO_MATCH;
    case AS608_ATT_BAD_IMAGE: return ATT_PIPE_BAD_IMAGE;
    case AS608_ATT_FATAL: return sensor_.faulted() ? ATT_PIPE_SENSOR_FAULT : ATT_PIPE_NOT_SAVED;
    case AS608_ATT_RETRYABLE: return ATT_PIPE_REJECTED;
    default: return ATT_PIPE_REJECTED;
  }
}

}  // namespace att
