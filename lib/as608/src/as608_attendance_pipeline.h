#pragma once
#include <stdint.h>
#include "as608_attendance_client.h"
#include "config_store.h"
#include "log_store.h"
#include "roster_store.h"
#include "day_view.h"

namespace att {

struct Occupancy;
class As608Runtime;

// Physical page and logical student ID are distinct namespaces. Translation is
// mandatory and injected deliberately. The current production resolver is provisional
// pending verification of the installed sensor's search and index-page semantics.
class IAs608PageResolver : public ISensorPageMap {
 public:
  virtual ~IAs608PageResolver() {}
};

// Product-level admission belongs to the serialized application loop. It must reject
// no-class and already-closed sessions. Keeping it explicit prevents this adapter from
// silently inventing those policies or maintaining a stale DayCache.
class IAttendanceAdmission {
 public:
  virtual ~IAttendanceAdmission() {}
  virtual bool allowScan(int32_t localDay, uint8_t session) const = 0;
  // Called after durable commit as part of the same serialized update sequence; the
  // caller immediately applies ClosureEngine::setSchedule() before processing resumes.
  virtual bool canSetParamsAfterCommit(const Params& params) const = 0;
  virtual bool setParamsAfterCommit(const Params& params) = 0;
  // Optional duplicate suppression. A production log-backed policy overrides this.
  virtual bool alreadyScanned(int32_t, uint8_t, uint16_t) const { return false; }
  // Bounded non-scan-loop work (for example, constructing a missing day view).
  virtual void service() {}
};

// Concrete fail-closed admission backed by the append-only event log. It updates a
// ready day/session view incrementally; a cache miss is rebuilt in bounded service
// slices. Until complete, admission is denied and duplicate checks conservatively block.
// A reset, log fault, failed replay, or observer-capacity failure blocks attendance.
// One instance must be attached to the
// same LogStore used by the pipeline and remain alive while that LogStore can notify it.
class LogAttendanceAdmission : public IAttendanceAdmission, public LogObserver {
 public:
  LogAttendanceAdmission(LogStore& log, const Params& params,
                         bool attachImmediately = true);
  LogAttendanceAdmission(const LogAttendanceAdmission&) = delete;
  LogAttendanceAdmission& operator=(const LogAttendanceAdmission&) = delete;
  bool attached() const { return attached_ || log_.hasObserver(this); }
  bool usesLog(const LogStore& log) const { return &log_ == &log; }
  bool usesParams(const Params& params) const {
    return params_.utcOffsetMin == params.utcOffsetMin &&
        params_.lateMin == params.lateMin &&
        params_.scanLeadMin == params.scanLeadMin;
  }
  bool allowScan(int32_t localDay, uint8_t session) const override;
  bool canSetParamsAfterCommit(const Params& params) const override;
  bool setParamsAfterCommit(const Params& params) override;
  bool alreadyScanned(int32_t localDay, uint8_t session, uint16_t studentId) const override;
  void service() override { (void)serviceViewBuild(kRecordsPerService); }
  bool serviceViewBuild(size_t maxRecords);
  bool viewBuildPending() const { return building_; }
  bool healthy() const override { return !buildFailed_; }
  void onReset() override;
  void onRecord(const Event& event) override;
  void onAttached() override { attached_ = true; }
  bool canAttach() const override { return !attached_; }
 private:
  static const size_t kRecordsPerService = 8;
  bool viewFor_(int32_t localDay, uint8_t session) const;
  void beginViewBuild_(int32_t localDay, uint8_t session) const;
  void failViewBuild_() const;
  LogStore& log_;
  Params params_;
  mutable DayView view_;
  mutable int32_t day_;
  mutable uint8_t session_;
  mutable bool viewValid_;
  mutable bool building_;
  mutable bool buildStale_;
  mutable bool buildFailed_;
  mutable size_t buildCursor_;
  mutable size_t buildCount_;
  mutable uint64_t buildGeneration_;
  bool attached_;
};

enum AttendancePipelineResult : uint8_t {
  ATT_PIPE_IDLE,
  ATT_PIPE_PENDING,
  ATT_PIPE_SAVED_PRESENT,
  ATT_PIPE_SAVED_LATE,
  ATT_PIPE_ALREADY_RECORDED,
  ATT_PIPE_NO_FINGER,
  ATT_PIPE_NO_MATCH,
  ATT_PIPE_BAD_IMAGE,
  ATT_PIPE_OUTSIDE_WINDOW,
  ATT_PIPE_NOT_ACTIVE,
  ATT_PIPE_REJECTED,
  ATT_PIPE_NOT_SAVED,          // no durable append was attempted
  ATT_PIPE_SENSOR_FAULT,
  ATT_PIPE_COMMIT_UNCERTAIN     // append returned false; storage may contain none, some or all bytes
};

// Converts a completed physical sensor match into one durable EV_SCAN event.
// Both begin() and commit require append-ready storage, not merely a non-faulted LogStore.
// Calls are serialized with ClosureEngine/config changes on the application loop.
// The pipeline owns no sensor resources; As608AttendanceClient owns the full scan lease.
class As608AttendancePipeline {
 public:
  // Preferred runtime-bound construction: raw attendance-client mutation remains
  // private to this pipeline and the startup coordinator.
  As608AttendancePipeline(As608Runtime& runtime, LogStore& log,
      RosterStore& roster, const Config& config,
      const IAs608PageResolver& pages, IAttendanceAdmission& admission);
  // Low-level client-bound construction remains for isolated tests/adapters.
  As608AttendancePipeline(As608AttendanceClient& sensor, LogStore& log,
      RosterStore& roster, const Occupancy& occupancy, const Config& config,
      const IAs608PageResolver& pages, IAttendanceAdmission& admission);
  As608AttendancePipeline(const As608AttendancePipeline&) = delete;
  As608AttendancePipeline& operator=(const As608AttendancePipeline&) = delete;

  // Call after the durable commit, before ClosureEngine::setSchedule(), in the same
  // serialized config-apply operation. Do not run step() between these updates.
  bool canSetConfigAfterCommit(const Config& config) const;
  bool setConfigAfterCommit(const Config& config);
  bool begin(uint32_t nowEpoch);
  AttendancePipelineResult step();
  bool active() const { return active_; }
  bool faulted() const { return faulted_; }
  uint16_t lastStudentId() const { return lastStudentId_; }
  // Timestamp captured when the current/last scan attempt began. For committed
  // EV_SCAN records this is exactly the persisted event epoch.
  uint32_t scanEpoch() const { return scanEpoch_; }

 private:
  AttendancePipelineResult saveMatch_(uint16_t physicalPage, uint16_t score);
  As608AttendanceClient& sensor_;
  LogStore& log_;
  RosterStore& roster_;
  const Occupancy& occupancy_;
  Config config_;
  const IAs608PageResolver& pages_;
  IAttendanceAdmission& admission_;
  uint32_t scanEpoch_;
  uint16_t lastStudentId_;
  bool active_;
  bool faulted_;  // requires log replay/reconstruction after any ambiguous commit
};

}  // namespace att
