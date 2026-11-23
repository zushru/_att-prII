#pragma once
#include <stdint.h>
#include "as608_startup.h"
#include "application_attendance_gate.h"

namespace att {

enum As608ClearTemplatesStatus : uint8_t {
  AS608_CLEAR_IDLE = 0,
  AS608_CLEAR_BUSY,
  AS608_CLEAR_DONE,
  AS608_CLEAR_FAILED
};

// Preferred single-sensor attendance/enrollment composition for the production root. Owns one
// As608Runtime (including its occupancy map), its startup coordinator and checked pipeline.
// Do not construct multiple owners for one physical sensor. All calls must be made
// from one serialized application loop; this class does not add an RTOS mutex.
//
// LogStore, RosterStore, page resolver and admission policy are non-owning dependencies
// and must outlive this owner. Enrollment is exposed only through this owner so the
// single sensor arbiter, FSM and full occupancy refresh cannot be bypassed. Production
// HTTP authentication is enforced by the application root; physical search/index-page
// behavior remains unverified on the target module.
class As608AttendanceOwner {
 public:
  As608AttendanceOwner(Stream& stream, LogStore& log, RosterStore& roster,
      const Config& config, const IAs608PageResolver& pages,
      ApplicationAttendanceGate& gate, uint32_t sensorPassword);
  As608AttendanceOwner(Stream& stream, LogStore& log, RosterStore& roster,
      Occupancy& occupancy, const Config& config,
      const IAs608PageResolver& pages, ApplicationAttendanceGate& gate,
      uint32_t sensorPassword);
  As608AttendanceOwner(const As608AttendanceOwner&) = delete;
  As608AttendanceOwner& operator=(const As608AttendanceOwner&) = delete;

  // Call only after storage/config boot gates are complete.
  bool beginStartup();
  As608StartupStatus stepStartup();
  bool startupReady() const { return startup_.ready(); }
  bool startupFaulted() const { return startup_.faulted(); }
  bool matchesClosure(const ClosureEngine& closure) const {
    return gate_.matchesClosure(closure);
  }
  uint16_t sensorCapacity() const { return startup_.capacity(); }
  bool transportFaulted() const { return runtime_.transportFaulted(); }
  const Occupancy& occupancy() const { return runtime_.occupancy(); }

  // The only public attendance operation path; raw sensor client access is not exposed.
  bool beginScan(uint32_t nowEpoch);
  AttendancePipelineResult stepScan();
  uint16_t lastScanStudentId() const { return pipeline_.lastStudentId(); }
  uint32_t lastScanEpoch() const { return pipeline_.scanEpoch(); }
  // Call on every serialized application-loop iteration, including while idle.
  void service();
  bool scanActive() const { return pipeline_.active(); }
  bool attendanceFaulted() const { return pipeline_.faulted(); }

  // Administrative maintenance: acquires the same arbiter used by scans and
  // enrollment, issues AS608 EmptyDatabase, and verifies every occupied index
  // bit is clear before releasing the sensor. Failure retains the reservation.
  bool beginClearAllTemplates();
  As608ClearTemplatesStatus stepClearAllTemplates();
  As608ClearTemplatesStatus clearTemplatesStatus() const { return clearStatus_; }

  // Enrollment is a sensor-template operation, not a roster mutation. The caller
  // must first commit the student's identity to the event-sourced roster. One
  // owner holds the sensor lease for the complete batch; service() advances at
  // most one sensor operation per call and performs a full occupancy refresh
  // after an ambiguous store result before the FSM may continue.
  EnrollStart beginEnrollment(bool verifyAfterEnroll);
  bool retryEnrollment();
  bool tryDifferentEnrollmentFinger();
  bool skipEnrollmentStudent();
  bool retryEnrollmentVerification();
  bool continueAfterEnrollmentVerificationFailure();
  bool retryEnrollmentCancellation();
  void cancelEnrollment();
  bool enrollmentActive() const { return runtime_.enrollment().held(); }
  bool enrollmentOperationPending() {
    return runtime_.enrollmentSensor().operationPending() ||
        runtime_.occupancyReader().active();
  }
  bool enrollmentFaulted() const { return enrollmentFaulted_ || runtime_.transportFaulted(); }
  bool enrollmentVerificationEnabled() const {
    return runtime_.enrollmentFsm().verificationEnabled();
  }
  EnrollState enrollmentState() const { return runtime_.enrollmentFsm().state(); }
  uint16_t enrollmentCurrentId() const { return runtime_.enrollmentFsm().currentId(); }
  uint16_t enrollmentDuplicateId() const { return runtime_.enrollmentFsm().duplicateId(); }
  uint16_t enrollmentDuplicateScore() const { return runtime_.enrollmentFsm().duplicateScore(); }
  uint16_t enrollmentQueueSize() const { return runtime_.enrollmentFsm().queueSize(); }
  uint16_t enrollmentQueuePosition() const { return runtime_.enrollmentFsm().queuePosition(); }
  uint16_t enrollmentCount() const { return runtime_.enrollmentFsm().enrolledCount(); }
  uint16_t enrollmentVerifiedCount() const { return runtime_.enrollmentFsm().verifiedCount(); }
  uint16_t enrollmentVerificationFailedCount() const { return runtime_.enrollmentFsm().verificationFailedCount(); }
  uint16_t enrollmentSkippedCount() const { return runtime_.enrollmentFsm().skippedCount(); }

  // Preserve the existing config-apply ordering contract: durable config commit first,
  // then update admission/pipeline here, then update ClosureEngine before more steps.
  bool canSetConfigAfterCommit(const Config& config) const;
  bool setConfigAfterCommit(const Config& config);

 private:
  ApplicationAttendanceGate& gate_;
  bool bindingsValid_;
  As608Runtime runtime_;
  As608StartupCoordinator startup_;
  As608AttendancePipeline pipeline_;
  bool enrollmentFaulted_;
  As608ClearTemplatesStatus clearStatus_;
  uint8_t clearPhase_;
  uint8_t clearPage_;
};

}  // namespace att
