#include "as608_attendance_pipeline.h"
#include <string.h>
namespace att {

LogAttendanceAdmission::LogAttendanceAdmission(LogStore& log,
                                               const Params& params,
                                               bool attachImmediately)
    : log_(log), params_(params), day_(-1), session_(0), viewValid_(false),
      building_(false), buildStale_(false), buildFailed_(false),
      buildCursor_(0), buildCount_(0), buildGeneration_(0), attached_(false) {
  if (attachImmediately) attached_ = log_.attach(this);
}

void LogAttendanceAdmission::onReset() {
  viewValid_ = false;
  building_ = false;
  buildStale_ = false;
  buildFailed_ = false;
  buildCursor_ = 0;
  buildCount_ = 0;
  buildGeneration_ = 0;
  day_ = -1;
  session_ = 0;
}

void LogAttendanceAdmission::onRecord(const Event& event) {
  if (viewValid_) {
    applyEvent(view_, event, params_);
  } else if (building_) {
    // Preserve log order: do not apply an append ahead of the unfinished prefix.
    buildStale_ = true;
  }
}

bool LogAttendanceAdmission::canSetParamsAfterCommit(const Params& params) const {
  return attached() && !log_.faulted() && healthy() &&
      params.utcOffsetMin >= -720 && params.utcOffsetMin <= 840 &&
      params.scanLeadMin <= 60 && params.lateMin <= 240;
}

bool LogAttendanceAdmission::setParamsAfterCommit(const Params& params) {
  if (!canSetParamsAfterCommit(params)) return false;
  params_ = params;
  viewValid_ = false;
  building_ = false;
  buildStale_ = false;
  buildCursor_ = 0;
  buildCount_ = 0;
  buildGeneration_ = 0;
  day_ = -1;
  session_ = 0;
  return true;
}

void LogAttendanceAdmission::beginViewBuild_(int32_t localDay,
                                              uint8_t session) const {
  viewValid_ = false;
  building_ = true;
  buildStale_ = false;
  buildCursor_ = 0;
  buildCount_ = log_.count();
  buildGeneration_ = log_.openGeneration();
  day_ = localDay;
  session_ = session;
  memset(&view_, 0, sizeof view_);
  view_.day = localDay;
  view_.session = session;
}

void LogAttendanceAdmission::failViewBuild_() const {
  viewValid_ = false;
  building_ = false;
  buildStale_ = false;
  buildFailed_ = true;
}

bool LogAttendanceAdmission::serviceViewBuild(size_t maxRecords) {
  if (viewValid_) return true;
  if (!building_ || buildFailed_ || maxRecords == 0 || !attached() ||
      log_.faulted() || log_.openGeneration() != buildGeneration_) {
    if (building_ && (log_.faulted() ||
        log_.openGeneration() != buildGeneration_)) failViewBuild_();
    return false;
  }
  if (buildStale_ || log_.count() != buildCount_) {
    beginViewBuild_(day_, session_);
    return false;
  }
  size_t processed = 0;
  Event event;
  while (buildCursor_ < buildCount_ && processed < maxRecords) {
    if (!log_.at(buildCursor_, event)) {
      failViewBuild_();
      return false;
    }
    applyEvent(view_, event, params_);
    ++buildCursor_;
    ++processed;
  }
  if (buildStale_ || log_.count() != buildCount_) {
    beginViewBuild_(day_, session_);
    return false;
  }
  if (buildCursor_ == buildCount_) {
    if (log_.faulted() || log_.openGeneration() != buildGeneration_) {
      failViewBuild_();
      return false;
    }
    building_ = false;
    viewValid_ = true;
    return true;
  }
  return false;
}

bool LogAttendanceAdmission::viewFor_(int32_t localDay, uint8_t session) const {
  if (!attached() || log_.faulted() || buildFailed_ ||
      localDay < 0 || session > 1) return false;
  if (viewValid_ && day_ == localDay && session_ == session) return true;
  if (!building_ || day_ != localDay || session_ != session)
    beginViewBuild_(localDay, session);
  return false;
}

bool LogAttendanceAdmission::allowScan(int32_t localDay, uint8_t session) const {
  if (!viewFor_(localDay, session)) return false;
  return !view_.noClassDay && !view_.noClassSession && !view_.closed;
}

bool LogAttendanceAdmission::alreadyScanned(int32_t localDay, uint8_t session,
                                            uint16_t studentId) const {
  if (studentId < 1 || studentId > MAX_ID) return false;
  if (!viewFor_(localDay, session)) return true;
  const uint8_t status = view_.scan[studentId];
  return status == ST_PRESENT || status == ST_LATE;
}

}  // namespace att
