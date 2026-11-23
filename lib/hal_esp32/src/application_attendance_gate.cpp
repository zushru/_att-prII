#include "application_attendance_gate.h"

namespace att {

ApplicationAttendanceGate::ApplicationAttendanceGate(
    const ConfigBootCoordinator& configBoot,
    LittleFsStorageOwner& storageOwner, const MonthSet& months,
    LogStore& log, RosterStore& roster, LogAttendanceAdmission& admission,
    ClosureEngine& closure, const HealthFsm& health)
    : ApplicationAttendanceGate(configBoot, storageOwner, months, log, roster,
                                 roster, admission, closure, health) {}

ApplicationAttendanceGate::ApplicationAttendanceGate(
    const ConfigBootCoordinator& configBoot,
    LittleFsStorageOwner& storageOwner, const MonthSet& months,
    LogStore& log, RosterStore& roster, IRoster& closureRoster,
    LogAttendanceAdmission& admission, ClosureEngine& closure,
    const HealthFsm& health)
    : configBoot_(configBoot), storageOwner_(storageOwner), months_(months),
      log_(log), roster_(roster), closureRoster_(closureRoster),
      admission_(admission), closure_(closure), health_(health) {}

bool ApplicationAttendanceGate::storageAndReplayReady_() const {
  if (!configBoot_.ready() ||
      storageOwner_.state() != LITTLEFS_OWNER_READY) return false;
  IFileSet* files = storageOwner_.fileSet();
  if (!files || !months_.usesFileSet(*files) ||
      !log_.usesStorage(months_) || log_.faulted()) return false;
  if (!roster_.historyComplete() || !log_.hasObserver(&roster_)) return false;
  if (!admission_.attached() || !admission_.usesLog(log_) ||
      !log_.hasObserver(&admission_)) return false;
  if (!closure_.ready() ||
      !closure_.usesLog(log_) || !closure_.usesRoster(closureRoster_)) return false;
  return true;
}

bool ApplicationAttendanceGate::authorizeStorageWritesAfterReplay() {
  // Clock audit must be writable while clock trust is being repaired, and
  // operator recovery must remain possible during an isolated sensor fault.
  // Config/storage/unprovisioned health faults are never write-authorized.
  const HState state = health_.state();
  if (state != H_OK && state != H_CLOCK_FAULT &&
      state != H_SENSOR_FAULT) return false;
  if (!storageAndReplayReady_()) return false;
  return storageOwner_.enableWritesAfterReplay_();
}

bool ApplicationAttendanceGate::readyForSensorStartup() const {
  if (!storageAndReplayReady_()) return false;
  // Sensor authentication, capacity and index reconciliation are read-only.
  // They remain useful while the clock is awaiting explicit trust confirmation,
  // or while the sensor itself has not yet passed startup. Storage/config/log/
  // roster/closure structure must still be valid.
  const HState state = health_.state();
  return state == H_OK || state == H_CLOCK_FAULT ||
      state == H_SENSOR_FAULT;
}

bool ApplicationAttendanceGate::readyForAttendance() const {
  // A structurally valid read-only mount is enough for sensor reconciliation,
  // but never enough to authorize an attendance event that must be committed.
  if (!storageAndReplayReady_() || closure_.pending() ||
      !storageOwner_.writable() || health_.state() != H_OK) return false;
  return HealthFsm::perms(health_.state()).scan;
}

bool ApplicationAttendanceGate::matches(const LogStore& log,
                                          const RosterStore& roster) const {
  return &log_ == &log && &roster_ == &roster;
}

bool ApplicationAttendanceGate::matchesConfig(const Config& config) const {
  const Config* bootConfig = configBoot_.config();
  return bootConfig && configEqual(*bootConfig, config) &&
      roster_.utcOffsetMin() == config.params.utcOffsetMin &&
      admission_.usesParams(config.params) &&
      closure_.matchesConfig(config);
}

bool ApplicationAttendanceGate::allowScan(int32_t localDay,
                                           uint8_t session) const {
  return readyForAttendance() && admission_.allowScan(localDay, session);
}

bool ApplicationAttendanceGate::canSetParamsAfterCommit(
    const Params& params) const {
  return readyForAttendance() && admission_.canSetParamsAfterCommit(params);
}

bool ApplicationAttendanceGate::setParamsAfterCommit(const Params& params) {
  return readyForAttendance() && admission_.setParamsAfterCommit(params);
}

bool ApplicationAttendanceGate::alreadyScanned(int32_t localDay,
    uint8_t session, uint16_t studentId) const {
  return readyForAttendance() &&
      admission_.alreadyScanned(localDay, session, studentId);
}

void ApplicationAttendanceGate::service() {
  if (storageAndReplayReady_()) admission_.service();
}

}  // namespace att
