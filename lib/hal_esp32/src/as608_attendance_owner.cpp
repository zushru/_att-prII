#include "as608_attendance_owner.h"

namespace att {

As608AttendanceOwner::As608AttendanceOwner(Stream& stream, LogStore& log,
    RosterStore& roster, const Config& config, const IAs608PageResolver& pages,
    ApplicationAttendanceGate& gate, uint32_t sensorPassword)
    : gate_(gate),
      bindingsValid_(gate.matches(log, roster) && gate.matchesConfig(config)),
      runtime_(stream, roster, sensorPassword, &pages),
      startup_(runtime_),
      pipeline_(runtime_, log, roster, config, pages, gate),
      enrollmentFaulted_(false), clearStatus_(AS608_CLEAR_IDLE), clearPhase_(0),
      clearPage_(0) {}


As608AttendanceOwner::As608AttendanceOwner(Stream& stream, LogStore& log,
    RosterStore& roster, Occupancy& occupancy, const Config& config,
    const IAs608PageResolver& pages, ApplicationAttendanceGate& gate,
    uint32_t sensorPassword)
    : gate_(gate),
      bindingsValid_(gate.matches(log, roster) && gate.matchesConfig(config)),
      runtime_(stream, roster, occupancy, sensorPassword, &pages),
      startup_(runtime_),
      pipeline_(runtime_, log, roster, config, pages, gate),
      enrollmentFaulted_(false), clearStatus_(AS608_CLEAR_IDLE), clearPhase_(0),
      clearPage_(0) {}

bool As608AttendanceOwner::beginStartup() {
  return bindingsValid_ && gate_.readyForSensorStartup() && startup_.begin();
}

As608StartupStatus As608AttendanceOwner::stepStartup() {
  return startup_.step();
}

void As608AttendanceOwner::service() {
  if (!bindingsValid_ || !startup_.ready()) return;
  gate_.service();
  if (runtime_.enrollment_.held() && !enrollmentFaulted_) {
    EnrollFsm& fsm = runtime_.enrollmentFsm();
    if (fsm.state() == EN_OCCUPANCY_REFRESH_REQUIRED) {
      As608OccupancyReader& reader = runtime_.occupancyReader();
      if (!reader.active() && !reader.begin()) {
        enrollmentFaulted_ = true;
        return;
      }
      const EnrollIo refresh = reader.step();
      if (refresh == EIO_PENDING) return;
      if (refresh != EIO_OK || !fsm.resumeAfterOccupancyRefresh()) {
        // An incomplete or failed full-index read must never release the sensor
        // to attendance. Keep the enrollment lease held until a fresh reboot.
        enrollmentFaulted_ = true;
        return;
      }
    } else {
      fsm.step();
    }
    if (runtime_.transportFaulted()) enrollmentFaulted_ = true;
    // sync() returns true only when EN_DONE/EN_CANCELLED and every UART/index
    // operation is drained. False during an ordinary in-progress batch is normal.
    (void)runtime_.enrollment_.sync();
  }
}

EnrollStart As608AttendanceOwner::beginEnrollment(bool verifyAfterEnroll) {
  if (!bindingsValid_ || !startup_.ready() || enrollmentFaulted_ ||
      runtime_.transportFaulted() || !gate_.readyForSensorStartup() ||
      pipeline_.active() || pipeline_.faulted())
    return ES_BEGIN_BUSY;
  if (!runtime_.enrollment_.acquire()) return ES_BEGIN_BUSY;
  const EnrollStart result = runtime_.enrollmentFsm().begin(
      startup_.capacity(), verifyAfterEnroll);
  if (result == ES_BEGIN_NO_WORK) {
    if (!runtime_.enrollment_.sync()) enrollmentFaulted_ = true;
    return result;
  }
  if (result != ES_BEGIN_OK) {
    if (!runtime_.enrollment_.abortPreparation()) enrollmentFaulted_ = true;
    return result;
  }
  return result;
}

bool As608AttendanceOwner::retryEnrollment() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() && runtime_.enrollmentFsm().retry();
}

bool As608AttendanceOwner::tryDifferentEnrollmentFinger() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() && runtime_.enrollmentFsm().tryDifferentFinger();
}

bool As608AttendanceOwner::skipEnrollmentStudent() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() && runtime_.enrollmentFsm().skipCurrent();
}

bool As608AttendanceOwner::retryEnrollmentVerification() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() && runtime_.enrollmentFsm().retryVerification();
}

bool As608AttendanceOwner::continueAfterEnrollmentVerificationFailure() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() &&
      runtime_.enrollmentFsm().continueAfterVerificationFailure();
}

bool As608AttendanceOwner::retryEnrollmentCancellation() {
  return enrollmentActive() && !enrollmentFaulted_ &&
      !runtime_.transportFaulted() && runtime_.enrollmentFsm().retryCancelRelease();
}

void As608AttendanceOwner::cancelEnrollment() {
  if (!enrollmentActive() || enrollmentFaulted_) return;
  if (runtime_.transportFaulted()) {
    // A poisoned link cannot prove finger release. Retain the arbiter lease.
    enrollmentFaulted_ = true;
    return;
  }
  runtime_.enrollmentFsm().cancel();
}

bool As608AttendanceOwner::beginClearAllTemplates() {
  if (!bindingsValid_ || !startup_.ready() || enrollmentFaulted_ ||
      runtime_.transportFaulted() || runtime_.enrollment_.held() ||
      pipeline_.active() || pipeline_.faulted() ||
      clearStatus_ == AS608_CLEAR_BUSY)
    return false;
  if (!runtime_.arbiter_.reserveEnrollment()) return false;

  // EmptyDatabase is destructive and may have an ambiguous outcome after a
  // timeout, transport failure, or rejected readback. Invalidate the cached
  // occupancy snapshot before issuing it so stale pre-clear identities cannot
  // survive any failed or partially completed maintenance attempt.
  runtime_.mutableOccupancy().begin();

  clearPage_ = 0;
  clearPhase_ = 0;
  clearStatus_ = AS608_CLEAR_BUSY;
  const EnrollIo started = runtime_.enrollmentSensor().emptyDatabase();
  if (started == EIO_PENDING) return true;
  if (started == EIO_OK) {
    clearPhase_ = 1;
    return true;
  }
  clearStatus_ = AS608_CLEAR_FAILED;
  enrollmentFaulted_ = true;
  return false;
}

As608ClearTemplatesStatus As608AttendanceOwner::stepClearAllTemplates() {
  if (clearStatus_ != AS608_CLEAR_BUSY) return clearStatus_;
  if (runtime_.transportFaulted()) {
    clearStatus_ = AS608_CLEAR_FAILED;
    enrollmentFaulted_ = true;
    return clearStatus_;
  }

  if (clearPhase_ == 0) {
    const EnrollIo result = runtime_.enrollmentSensor().emptyDatabase();
    if (result == EIO_PENDING) return clearStatus_;
    if (result != EIO_OK) {
      clearStatus_ = AS608_CLEAR_FAILED;
      enrollmentFaulted_ = true;
      return clearStatus_;
    }
    clearPhase_ = 1;
    clearPage_ = 0;
  }

  if (clearPhase_ == 1) {
    uint8_t bitmap[32] = {};
    const EnrollIo result = runtime_.enrollmentSensor().readIndexPage(
        clearPage_, bitmap);
    if (result == EIO_PENDING) return clearStatus_;
    if (result != EIO_OK) {
      clearStatus_ = AS608_CLEAR_FAILED;
      enrollmentFaulted_ = true;
      return clearStatus_;
    }
    const uint16_t capacity = startup_.capacity();
    const uint16_t firstSlot = static_cast<uint16_t>(clearPage_) * 256u;
    const uint16_t slotsOnPage = capacity > firstSlot
        ? static_cast<uint16_t>(capacity - firstSlot) : 0;
    const uint16_t checked = slotsOnPage < 256u ? slotsOnPage : 256u;
    for (uint16_t bit = 0; bit < checked; ++bit) {
      if ((bitmap[bit / 8u] & static_cast<uint8_t>(1u << (bit % 8u))) != 0) {
        clearStatus_ = AS608_CLEAR_FAILED;
        enrollmentFaulted_ = true;
        return clearStatus_;
      }
    }
    ++clearPage_;
    if (clearPage_ < Occupancy::pagesNeeded(capacity)) return clearStatus_;

    // The empty bitmap is not used for attendance before reboot; invalidate
    // the cached snapshot so no stale pre-reset occupancy can be trusted.
    runtime_.mutableOccupancy().begin();
    if (!runtime_.arbiter_.releaseEnrollment()) {
      clearStatus_ = AS608_CLEAR_FAILED;
      enrollmentFaulted_ = true;
      return clearStatus_;
    }
    clearStatus_ = AS608_CLEAR_DONE;
  }
  return clearStatus_;
}

bool As608AttendanceOwner::beginScan(uint32_t nowEpoch) {
  return bindingsValid_ && startup_.ready() &&
      gate_.readyForAttendance() && pipeline_.begin(nowEpoch);
}

AttendancePipelineResult As608AttendanceOwner::stepScan() {
  if (!startup_.ready()) return ATT_PIPE_REJECTED;
  return pipeline_.step();
}

bool As608AttendanceOwner::canSetConfigAfterCommit(const Config& config) const {
  return bindingsValid_ && startup_.ready() && !enrollmentActive() &&
      gate_.readyForAttendance() &&
      pipeline_.canSetConfigAfterCommit(config);
}

bool As608AttendanceOwner::setConfigAfterCommit(const Config& config) {
  return bindingsValid_ && startup_.ready() && !enrollmentActive() &&
      gate_.readyForAttendance() &&
      pipeline_.setConfigAfterCommit(config);
}

}  // namespace att
