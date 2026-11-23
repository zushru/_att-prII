#include <Arduino.h>

// Serial2 is reserved for the AS608 sensor; the product has no USB console.
#include <WebServer.h>
#include <esp_timer.h>
#include <esp_system.h>
#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#endif
#include <new>
#include <stdio.h>
#include <stdint.h>
#include <Wire.h>
#include <string.h>
#include <nvs.h>

#include "dashboard_html.h"

#include "ap_credential_bootstrap.h"
#include "ap_credential_setup.h"
#include "setup_code_sequence.h"
#include "esp32_first_run_http_handler.h"
#include "esp32_first_run_clock_provisioner.h"
#include "esp32_nvs_closure_checkpoint_store.h"
#include "esp32_first_run_closure_checkpoint_finalizer.h"
#include "esp32_admin_auth_http_handler.h"
#include "esp32_admin_roster_http_handler.h"
#include "admin_session.h"
#include "config_boot.h"
#include "esp32_bootstrap_random.h"
#include "esp32_i2c_bus_owner.h"
#include "i2c_recovery_policy.h"
#include "esp32_ds3231_register_io.h"
#include "ds3231_clock.h"
#include "time_rules.h"
#include "month_set.h"
#include "log_store.h"
#include "event_group_audit.h"
#include "roster_store.h"
#include "roster_event_writer.h"
#include "as608_attendance_pipeline.h"
#include "as608_attendance_owner.h"
#include "as608_page_resolver.h"
#include "enrolled_roster.h"
#include "application_attendance_gate.h"
#include "clock_confirmation.h"
#include "clock_confirmation_request.h"
#include "clock_health_owner.h"
#include "esp32_admin_clock_http_handler.h"
#include "closure_engine.h"
#include "closure_checkpoint_coordinator.h"
#include "event_codec.h"
#include "loop_watchdog_policy.h"
#include "health_fsm.h"
#include "config_runtime_guard.h"
#include "esp32_littlefs_mount.h"
#include "esp32_nvs_startup_guard.h"
#include "esp32_oled.h"
#include "esp32_buzzer.h"
#include "oled_idle_carousel.h"
#include "esp32_pin_auth_runtime.h"
#include "esp32_clock_monotonic.h"
#include "esp32_ram_setup_code_verifier.h"
#include "esp32_mbedtls_pin_config_runtime.h"
#include "esp32_mbedtls_pin_writer.h"
#include "esp32_secure_random.h"
#include "esp32_recovery_presence.h"
#include "esp32_recovery_service.h"
#include "esp32_recovery_http_handler.h"
#include "esp32_wifi_ap.h"
#include "nvs_ap_credential_store.h"
#include "nvs_config_backend.h"
#include "littlefs_mount_owner.h"

namespace {

const uint32_t kI2cClockHz = 100000UL;
const int64_t kRtcHealthPollPeriodUs = 5000000LL;
const uint8_t kSdaPin = 21;
const uint8_t kSclPin = 22;
const char kLittleFsMountPoint[] = "/littlefs";
const uint32_t kAdminSessionReapPeriodS = 30UL;
const uint32_t kAs608Baud = 57600UL;
const int8_t kAs608RxPin = 16;
const int8_t kAs608TxPin = 17;
// AS608 TCH is the local finger-detection presence signal. Keep the GPIO
// input-only; never drive this module output from the ESP32.
const int8_t kAs608TchPin = 4;
const bool kAs608TchExpectedActiveHigh = true;
// User-directed logical mapping: physical sensor page 0 is Student ID 1.
// Keep both origins here; no other layer may translate page/ID arithmetic.
const uint16_t kAs608FirstPhysicalPage = 0;
const uint16_t kAs608FirstStudentId = 1;
// VerifyPassword only. This factory-default credential is never changed;
// a module with a different existing password fails closed at sensor startup.
const uint32_t kAs608FactoryPassword = 0x00000000UL;

enum RtcStartupObservation : uint8_t {
  RTC_STARTUP_NOT_CHECKED = 0,
  RTC_STARTUP_BUS_UNAVAILABLE,
  RTC_STARTUP_READ_FAILED,
  RTC_STARTUP_INVALID_CALENDAR,
  RTC_STARTUP_OSF_SET,
  RTC_STARTUP_BELOW_FLOOR,
  RTC_STARTUP_BELOW_LOG,
  RTC_STARTUP_CANDIDATE_UNTRUSTED,
  RTC_STARTUP_CONFIRMED
};

const char kNeedsSetupJson[] PROGMEM =
    "{\"boot\":\"needs_setup\",\"ap\":\"ready\","
    "\"storage\":\"not_started\",\"rtc_observation\":\"not_checked\","
    "\"clock_trusted\":false,\"attendance_enabled\":false,"
    "\"admin_routes_enabled\":false,\"admin_auth_routes_enabled\":false,"
    "\"config_provisioning_enabled\":false,"
    "\"config_provisioning_state\":\"unavailable\","
    "\"setup_code_remaining_seconds\":0,\"sf2_export_enabled\":false}";

class Esp32ApplicationRestart : public att::IApplicationRestart {
 public:
  void restartAfterProvisioning() override { esp_restart(); }
};

enum AdminMaintenanceAction : uint8_t {
  ADMIN_MAINTENANCE_NONE = 0,
  ADMIN_MAINTENANCE_RESTART,
  ADMIN_MAINTENANCE_CLEAR_RECORDS,
  ADMIN_MAINTENANCE_FACTORY_RESET
};

struct Application {
  att::Esp32NvsConfigBackend configBackend;
  att::ConfigBootCoordinator configBoot;
  att::Esp32NvsClosureCheckpointStore closureCheckpointStore;
  att::Esp32FirstRunClosureCheckpointFinalizer firstRunCheckpointFinalizer;
  att::Esp32PinAuthRuntime pinAuthRuntime;
  att::Esp32PinAuthRuntime setupCodeAuthRuntime;
  att::Esp32NvsApCredentialStore apCredentialStore;
  att::Esp32BootstrapRandom bootstrapRandom;
  att::ApCredentialBootstrap apCredentialBootstrap;
  att::Esp32SecureRandom runtimeRandom;
  att::Esp32GpioRecoveryPresence recoveryPresence;
  att::Esp32MbedTlsInitialPinPreparer initialPinPreparer;
  att::Esp32WifiAp wifiAp;
  att::Esp32I2cBusOwner i2cBus;
  att::Esp32Ds3231RegisterIo rtcRegisterIo;
  att::Ds3231Clock rtcClock;
  att::Esp32Oled oled;
  att::Esp32ClockMonotonicSource monotonicClock;
  att::Esp32FirstRunClockProvisioner firstRunClockProvisioner;
  att::Esp32RamSetupCodeVerifier setupCodeVerifier;
  alignas(att::Esp32MbedTlsPinConfigRuntime)
      uint8_t pinConfigRuntimeMemory[sizeof(att::Esp32MbedTlsPinConfigRuntime)];
  att::Esp32MbedTlsPinConfigRuntime* pinConfigRuntime;
  alignas(att::Esp32MbedTlsAdminPinWriter)
      uint8_t adminPinWriterMemory[sizeof(att::Esp32MbedTlsAdminPinWriter)];
  att::Esp32MbedTlsAdminPinWriter* adminPinWriter;
  alignas(att::AdminSessionBoundary)
      uint8_t adminSessionsMemory[sizeof(att::AdminSessionBoundary)];
  att::AdminSessionBoundary* adminSessions;
  alignas(att::Esp32AdminAuthHttpHandler)
      uint8_t adminAuthHttpHandlerMemory[sizeof(att::Esp32AdminAuthHttpHandler)];
  att::Esp32AdminAuthHttpHandler* adminAuthHttpHandler;
  alignas(att::Esp32AdminRosterHttpHandler)
      uint8_t adminRosterHttpHandlerMemory[sizeof(att::Esp32AdminRosterHttpHandler)];
  att::Esp32AdminRosterHttpHandler* adminRosterHttpHandler;
  att::ApCredentialSetupCoordinator apSetup;
  alignas(att::FirstRunSetupCodeSequence)
      uint8_t firstRunSequenceMemory[sizeof(att::FirstRunSetupCodeSequence)];
  att::FirstRunSetupCodeSequence* firstRunSequence;
  alignas(att::FirstRunSetupCodeBoundary)
      uint8_t firstRunSetupBoundaryMemory[sizeof(att::FirstRunSetupCodeBoundary)];
  att::FirstRunSetupCodeBoundary* firstRunSetupBoundary;
  alignas(att::Esp32FirstRunHttpHandler)
      uint8_t firstRunHttpHandlerMemory[sizeof(att::Esp32FirstRunHttpHandler)];
  att::Esp32FirstRunHttpHandler* firstRunHttpHandler;
  att::FirstRunProvisioningRestartGate firstRunRestart;
  Esp32ApplicationRestart applicationRestart;
  // The read-only initializer may format only a fully erased partition. The
  // normal mount is write-capable at the VFS layer, but its serialized file set
  // remains read-only until ApplicationAttendanceGate validates every replay.
  att::Esp32LittleFsMountBackend storageInitBackend;
  att::Esp32LittleFsMountBackend storageMountBackend;
  alignas(att::LittleFsStorageOwner)
      uint8_t storageOwnerMemory[sizeof(att::LittleFsStorageOwner)];
  att::LittleFsStorageOwner* storageOwner;
  alignas(att::ClockConfirmationService)
      uint8_t clockConfirmationServiceMemory[sizeof(att::ClockConfirmationService)];
  att::ClockConfirmationService* clockConfirmationService;
  alignas(att::AdminClockConfirmation)
      uint8_t adminClockConfirmationMemory[sizeof(att::AdminClockConfirmation)];
  att::AdminClockConfirmation* adminClockConfirmation;
  alignas(att::ClockHealthOwner)
      uint8_t clockHealthOwnerMemory[sizeof(att::ClockHealthOwner)];
  att::ClockHealthOwner* clockHealthOwner;
  alignas(att::Esp32AdminClockHttpHandler)
      uint8_t adminClockHttpHandlerMemory[sizeof(att::Esp32AdminClockHttpHandler)];
  att::Esp32AdminClockHttpHandler* adminClockHttpHandler;
  alignas(att::Esp32RecoveryService)
      uint8_t recoveryServiceMemory[sizeof(att::Esp32RecoveryService)];
  att::Esp32RecoveryService* recoveryService;
  alignas(att::Esp32RecoveryHttpHandler)
      uint8_t recoveryHttpHandlerMemory[sizeof(att::Esp32RecoveryHttpHandler)];
  att::Esp32RecoveryHttpHandler* recoveryHttpHandler;
  alignas(att::MonthSet) uint8_t monthSetMemory[sizeof(att::MonthSet)];
  att::MonthSet* months;
  alignas(att::LogStore) uint8_t logStoreMemory[sizeof(att::LogStore)];
  att::LogStore* log;
  att::EventGroupAudit eventGroupAudit;
  alignas(att::RosterStore)
      uint8_t rosterMemory[sizeof(att::RosterStore)];
  att::RosterStore* roster;
  att::Occupancy sensorOccupancy;
  alignas(att::SensorAwareRoster)
      uint8_t sensorAwareRosterMemory[sizeof(att::SensorAwareRoster)];
  att::SensorAwareRoster* sensorAwareRoster;
  alignas(att::RosterEventWriter)
      uint8_t rosterWriterMemory[sizeof(att::RosterEventWriter)];
  att::RosterEventWriter* rosterWriter;
  alignas(att::AttendanceEventWriter)
      uint8_t attendanceWriterMemory[sizeof(att::AttendanceEventWriter)];
  att::AttendanceEventWriter* attendanceWriter;
  alignas(att::LogAttendanceAdmission)
      uint8_t attendanceAdmissionMemory[sizeof(att::LogAttendanceAdmission)];
  att::LogAttendanceAdmission* attendanceAdmission;
  alignas(att::ClosureEngine)
      uint8_t closureMemory[sizeof(att::ClosureEngine)];
  att::ClosureEngine* closure;
  alignas(att::ClosureCheckpointCoordinator)
      uint8_t closureCoordinatorMemory[sizeof(att::ClosureCheckpointCoordinator)];
  att::ClosureCheckpointCoordinator* closureCoordinator;
  att::HealthFsm health;
  alignas(att::ApplicationAttendanceGate)
      uint8_t attendanceGateMemory[sizeof(att::ApplicationAttendanceGate)];
  att::ApplicationAttendanceGate* attendanceGate;
  att::LinearAs608PageResolver as608PageResolver;
  alignas(att::As608AttendanceOwner)
      uint8_t as608AttendanceOwnerMemory[sizeof(att::As608AttendanceOwner)];
  att::As608AttendanceOwner* as608AttendanceOwner;
  WebServer http;
  att::ConfigBootState bootState;
  bool oledReady;
  att::OledIdleCarousel idleCarousel;
  bool recoveryPresenceReady;
  bool apReady;
  bool httpReady;
  // Set only after the conditional first-run raw handler and its headers are
  // registered. READY boot never registers the provisioning route.
  bool firstRunSetupRouteReady;
  bool adminAuthRoutesReady;
  bool clockWritesAuthorized;
  bool clockConfirmationRoutesReady;
  bool storageReady;
  bool recoveryMode;
  bool eventGroupAuditReady;
  bool rosterHistoryReady;
  bool attendanceAdmissionAttached;
  bool closureReady;
  bool closureCheckpointLoaded;
  bool closureCheckpointFaulted;
  bool closureCoordinatorStartAttempted;
  bool closureCoordinatorStarted;
  bool closureCoordinatorFaulted;
  bool closureHealthObservationKnown;
  bool closureHealthHealthy;
  uint32_t closureLastHealthyEpoch;
  bool sensorStartupStarted;
  bool sensorStartupReady;
  bool sensorStartupFaulted;
  uint16_t sensorCapacity;
  RtcStartupObservation rtcObservation;
  bool rtcPresent;
  bool rtcLostPower;
  bool rtcCalendarValid;
  uint32_t rtcEpochUtc;
  // One validated DS3231 sample is shared by the serialized runtime services
  // within a loop pass. The cache is disabled around HTTP handlers and all
  // clock-mutating paths so no stale sample crosses an external state change.
  bool loopRtcCacheEnabled;
  bool loopRtcReadAttempted;
  att::Ds3231Reading loopRtcReading;
  att::Ds3231Code loopRtcReadCode;
  bool logReplayReady;
  int64_t nextProvisionClearRetryUs;
  int64_t nextRtcHealthPollUs;
  int64_t nextClosureServiceUs;
  int64_t nextAttendanceScanUs;
  int64_t scanFeedbackUntilUs;
  bool scanPromptVisible;
  uint32_t lastScanEpoch;
  uint16_t lastScanStudentId;
  const char* lastScanResult;
  bool enrollmentSoundStateKnown;
  att::EnrollState lastEnrollmentSoundState;
  att::I2cRecoveryPolicy i2cRecoveryPolicy;
  uint32_t i2cRecoveryAttempts;
  bool i2cRecoveryTerminalFault;
  uint8_t oledHealthMask;
  uint32_t lastAdminSessionReapS;
  uint32_t logRecordCount;
  uint32_t logBaselineEpoch;
  bool faulted;
  AdminMaintenanceAction maintenanceAction;
  const char* maintenanceError;

  Application()
      : configBoot(configBackend),
        closureCheckpointStore(configBoot),
        firstRunCheckpointFinalizer(configBoot, closureCheckpointStore),
        pinAuthRuntime("attsec", configBoot),
        setupCodeAuthRuntime("attsetup", configBoot),
        apCredentialStore(configBoot),
        apCredentialBootstrap(apCredentialStore, bootstrapRandom),
        recoveryPresence(kAs608TchPin, kAs608TchExpectedActiveHigh),
        initialPinPreparer(configBoot, runtimeRandom),
        wifiAp(runtimeRandom),
        i2cBus(Wire),
        rtcRegisterIo(Wire, i2cBus.lock()),
        rtcClock(rtcRegisterIo),
        oled(Wire, i2cBus.lock()),
        monotonicClock(),
        firstRunClockProvisioner(rtcClock, monotonicClock),
        setupCodeVerifier(runtimeRandom, monotonicClock, oled),
        pinConfigRuntime(nullptr),
        adminPinWriter(nullptr),
        adminSessions(nullptr),
        adminAuthHttpHandler(nullptr),
        adminRosterHttpHandler(nullptr),
        apSetup(configBoot, apCredentialBootstrap, oled, wifiAp),
        firstRunSequence(nullptr),
        firstRunSetupBoundary(nullptr),
        firstRunHttpHandler(nullptr),
        storageInitBackend(),
        storageMountBackend(true),
        storageOwner(nullptr),
        clockConfirmationService(nullptr),
        adminClockConfirmation(nullptr),
        clockHealthOwner(nullptr),
        adminClockHttpHandler(nullptr),
        recoveryService(nullptr),
        recoveryHttpHandler(nullptr),
        months(nullptr),
        log(nullptr),
        eventGroupAudit(),
        roster(nullptr),
        sensorOccupancy(),
        sensorAwareRoster(nullptr),
        rosterWriter(nullptr),
        attendanceWriter(nullptr),
        attendanceAdmission(nullptr),
        closure(nullptr),
        closureCoordinator(nullptr),
        health(),
        attendanceGate(nullptr),
        as608PageResolver(kAs608FirstPhysicalPage, kAs608FirstStudentId,
                          att::MAX_ID),
        as608AttendanceOwner(nullptr),
        http(80),
        bootState(att::CFG_BOOT_COLD),
        oledReady(false),
        recoveryPresenceReady(false),
        apReady(false),
        httpReady(false),
        firstRunSetupRouteReady(false),
        adminAuthRoutesReady(false),
        clockWritesAuthorized(false),
        clockConfirmationRoutesReady(false),
        storageReady(false),
        recoveryMode(false),
        eventGroupAuditReady(false),
        rosterHistoryReady(false),
        attendanceAdmissionAttached(false),
        closureReady(false),
        closureCheckpointLoaded(false),
        closureCheckpointFaulted(false),
        closureCoordinatorStartAttempted(false),
        closureCoordinatorStarted(false),
        closureCoordinatorFaulted(false),
        closureHealthObservationKnown(false),
        closureHealthHealthy(false),
        closureLastHealthyEpoch(0),
        sensorStartupStarted(false),
        sensorStartupReady(false),
        sensorStartupFaulted(false),
        sensorCapacity(0),
        rtcObservation(RTC_STARTUP_NOT_CHECKED),
        rtcPresent(false),
        rtcLostPower(false),
        rtcCalendarValid(false),
        rtcEpochUtc(0),
        loopRtcCacheEnabled(false),
        loopRtcReadAttempted(false),
        loopRtcReading{0, false},
        loopRtcReadCode(att::DS3231_IO_ERROR),
        logReplayReady(false),
        nextProvisionClearRetryUs(0),
        nextRtcHealthPollUs(0),
        nextClosureServiceUs(0),
        nextAttendanceScanUs(0),
        scanFeedbackUntilUs(0),
        scanPromptVisible(false),
        lastScanEpoch(0),
        lastScanStudentId(0),
        lastScanResult("waiting"),
        enrollmentSoundStateKnown(false),
        lastEnrollmentSoundState(att::EN_IDLE),
        i2cRecoveryPolicy(),
        i2cRecoveryAttempts(0),
        i2cRecoveryTerminalFault(false),
        oledHealthMask(0xFFu),
        lastAdminSessionReapS(0),
        logRecordCount(0),
        logBaselineEpoch(0),
        faulted(false),
        maintenanceAction(ADMIN_MAINTENANCE_NONE), maintenanceError("none") {}
};

// These health helpers are defined later in this translation unit but are also
// called by HTTP route callbacks registered during application initialization.
void updateRuntimeHealth(Application& app);
void showHealthStatusWhenChanged(Application& app);

const char* rtcStartupObservationText(RtcStartupObservation observation) {
  switch (observation) {
    case RTC_STARTUP_NOT_CHECKED: return "not_checked";
    case RTC_STARTUP_BUS_UNAVAILABLE: return "bus_unavailable";
    case RTC_STARTUP_READ_FAILED: return "read_failed";
    case RTC_STARTUP_INVALID_CALENDAR: return "invalid_calendar";
    case RTC_STARTUP_OSF_SET: return "oscillator_stopped";
    case RTC_STARTUP_BELOW_FLOOR: return "below_minimum_epoch";
    case RTC_STARTUP_BELOW_LOG: return "below_log_baseline";
    case RTC_STARTUP_CANDIDATE_UNTRUSTED:
      return "utc_candidate_untrusted";
    case RTC_STARTUP_CONFIRMED: return "confirmed";
    default: return "unknown";
  }
}

Application* gApp = nullptr;
// Kept outside Application so GPIO23 can be driven inactive before NVS startup
// checks or any potentially lengthy application initialization.
att::Esp32Buzzer gBuzzer;

bool currentClockHealthInputs(att::HealthInputs& inputs);

void serviceRecoveryLease(Application& app) {
  if (app.recoveryService == nullptr) return;
  if (app.recoveryService->cleanupPending() ||
      (app.recoveryService->active() && !app.recoveryPresence.asserted()))
    (void)app.recoveryService->end();
}

void secureClear(void* value, size_t length) {
  volatile uint8_t* cursor = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *cursor++ = 0;
}

uint32_t monotonicSeconds() {
  const int64_t elapsedUs = esp_timer_get_time();
  if (elapsedUs <= 0) return 0;
  return static_cast<uint32_t>(
      static_cast<uint64_t>(elapsedUs) / 1000000ULL);
}

uint16_t setupCodeRemainingSeconds(const Application& app) {
  const int64_t nowMicros = esp_timer_get_time();
  if (nowMicros < 0) return 0;
  return app.setupCodeVerifier.remainingSeconds(
      static_cast<uint64_t>(nowMicros));
}

const char* firstRunProvisioningBlocker(const Application& app) {
  if (app.firstRunSetupBoundary != nullptr &&
      app.firstRunSetupBoundary->completed()) {
    if (app.firstRunSetupBoundary->faulted())
      return "post_commit_fault_restart_pending";
    if (!app.firstRunSetupBoundary->restartReady())
      return "display_clear_pending";
    return "restart_required";
  }
  // Prefer the failing owner over the aggregate application latch so the
  // operator receives a useful, non-secret diagnosis instead of a generic
  // application_fault for every first-run startup failure.
  if (!app.runtimeRandom.rfStarted()) return "radio_not_ready";
  if (app.firstRunSequence == nullptr) return "sequence_missing";
  if (app.firstRunSequence->faulted()) return "sequence_fault";
  if (!app.setupCodeAuthRuntime.ready()) {
    switch (app.setupCodeAuthRuntime.code()) {
      case att::EPAR_COUNTER_READ_FAILED:
        return "setup_auth_counter_read_failed";
      case att::EPAR_COUNTER_INITIALIZE_FAILED:
        return "setup_auth_counter_initialize_failed";
      case att::EPAR_COORDINATOR_BOOT_FAILED:
        return "setup_auth_coordinator_boot_failed";
      case att::EPAR_CONFIG_NOT_READY:
        return "setup_auth_config_not_ready";
      case att::EPAR_ALREADY_STARTED:
        return "setup_auth_runtime_already_started";
      case att::EPAR_COLD:
      default:
        return "setup_auth_runtime_not_ready";
    }
  }
  if (app.firstRunSetupBoundary == nullptr) return "setup_boundary_missing";
  if (app.firstRunSetupBoundary->faulted()) return "setup_boundary_fault";
  if (app.setupCodeVerifier.faulted()) return "setup_code_verifier_fault";
  if (!app.firstRunSetupRouteReady) return "setup_route_not_registered";
  if (app.faulted) return "application_fault";
  return "none";
}

const char* firstRunProvisioningState(const Application& app,
                                      uint16_t remainingSeconds) {
  const char* blocker = firstRunProvisioningBlocker(app);
  if (strcmp(blocker, "display_clear_pending") == 0 ||
      strcmp(blocker, "post_commit_fault_restart_pending") == 0 ||
      strcmp(blocker, "restart_required") == 0)
    return "restart_pending";
  if (strcmp(blocker, "none") != 0) return "unavailable";

  if (app.firstRunSequence->ready() && app.setupCodeVerifier.active() &&
      remainingSeconds > 0) return "code_active";
  if (!app.setupCodeVerifier.readyToIssue()) return "reissuing";
  return "waiting_for_client";
}

void sendNeedsSetupStatus(Application& app) {
  const uint16_t observedRemainingSeconds = setupCodeRemainingSeconds(app);
  const char* state = firstRunProvisioningState(app, observedRemainingSeconds);
  const char* blocker = firstRunProvisioningBlocker(app);
  const bool restartPending = strcmp(state, "restart_pending") == 0;
  const bool enabled = strcmp(state, "unavailable") != 0 && !restartPending;
  const uint16_t remainingSeconds = strcmp(state, "code_active") == 0
      ? observedRemainingSeconds : 0;
  const bool boundaryCompleted = app.firstRunSetupBoundary != nullptr &&
      app.firstRunSetupBoundary->completed();
  const bool boundaryFaulted = app.firstRunSetupBoundary != nullptr &&
      app.firstRunSetupBoundary->faulted();
  char payload[768] = {};
  const int length = snprintf(
      payload, sizeof payload,
      "{\"boot\":\"needs_setup\",\"ap\":\"ready\","
      "\"storage\":\"not_started\",\"rtc_observation\":\"not_checked\","
      "\"clock_trusted\":false,\"attendance_enabled\":false,"
      "\"admin_routes_enabled\":false,\"admin_auth_routes_enabled\":false,"
        "\"config_provisioning_enabled\":%s,"
      "\"config_provisioning_state\":\"%s\","
      "\"config_provisioning_blocker\":\"%s\","
      "\"setup_auth_runtime_code\":%u,"
      "\"setup_auth_nvs_error\":%ld,"
      "\"setup_sequence_code\":%u,"
      "\"setup_boundary_completed\":%s,"
      "\"setup_boundary_faulted\":%s,"
      "\"setup_route_registered\":%s,"
      "\"setup_code_remaining_seconds\":%u,\"sf2_export_enabled\":false}",
      enabled ? "true" : "false", state, blocker,
      static_cast<unsigned>(app.setupCodeAuthRuntime.code()),
      static_cast<long>(app.setupCodeAuthRuntime.lastStoreError()),
      app.firstRunSequence != nullptr
          ? static_cast<unsigned>(app.firstRunSequence->code()) : 0u,
      boundaryCompleted ? "true" : "false",
      boundaryFaulted ? "true" : "false",
      app.firstRunSetupRouteReady ? "true" : "false",
      static_cast<unsigned>(remainingSeconds));
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    app.http.send(500, "application/json", "{}");
    return;
  }
  app.http.send(200, "application/json", payload);
}

void serviceAdminSessions(Application& app) {
  if (app.adminSessions == nullptr) return;

  const uint32_t nowS = monotonicSeconds();
  // Unsigned elapsed-time arithmetic remains valid across uint32_t wrap.
  if (static_cast<uint32_t>(nowS - app.lastAdminSessionReapS) <
      kAdminSessionReapPeriodS)
    return;

  app.lastAdminSessionReapS = nowS;
  // activeSessions() reaps expired records without refreshing idle deadlines.
  (void)app.adminSessions->activeSessions(nowS);
}

void observeRtcStartup(Application& app) {
  if (app.bootState != att::CFG_BOOT_READY) return;
  if (!app.i2cBus.ready()) {
    app.rtcObservation = RTC_STARTUP_BUS_UNAVAILABLE;
    return;
  }

  // E32: DS3231 calendar registers store UTC. Offset zero is mandatory here;
  // PHT conversion stays at the application boundary through Config.params.
  att::Ds3231Reading reading = {0, false};
  const att::Ds3231Code result = app.rtcClock.readUtcEpoch(0, reading);
  if (result == att::DS3231_IO_ERROR) {
    app.rtcObservation = RTC_STARTUP_READ_FAILED;
    return;
  }

  // INVALID_TIME is returned only after both register reads succeeded, so the
  // RTC answered even though its calendar fields cannot be accepted.
  app.rtcPresent = true;
  if (result == att::DS3231_INVALID_TIME) {
    app.rtcObservation = RTC_STARTUP_INVALID_CALENDAR;
    return;
  }
  if (result != att::DS3231_OK) {
    app.rtcObservation = RTC_STARTUP_READ_FAILED;
    app.rtcPresent = false;
    return;
  }

  app.rtcCalendarValid = true;
  app.rtcLostPower = reading.oscillatorStopped;
  app.rtcEpochUtc = reading.epochUtc;
  if (reading.oscillatorStopped) {
    app.rtcObservation = RTC_STARTUP_OSF_SET;
  } else if (reading.epochUtc < att::EPOCH_2020) {
    app.rtcObservation = RTC_STARTUP_BELOW_FLOOR;
  } else {
    // A plausible UTC candidate remains untrusted until the completed clock
    // journal and replayed log baseline are reconciled by the health owner.
    app.rtcObservation = RTC_STARTUP_CANDIDATE_UNTRUSTED;
  }
}

const char* healthStateText(att::HState state) {
  switch (state) {
    case att::H_OK: return "ok";
    case att::H_UNPROVISIONED: return "unprovisioned";
    case att::H_CLOCK_FAULT: return "clock_fault";
    case att::H_STORAGE_FAULT: return "storage_fault";
    case att::H_CONFIG_FAULT: return "config_fault";
    case att::H_SENSOR_FAULT: return "sensor_fault";
    default: return "unknown";
  }
}

const char* sensorStartupText(const Application& app);
const char* enrollStateText(att::EnrollState state);
bool attendanceRuntimeReady(const Application& app);

void sendConfiguredStatus(Application& app) {
  const bool clockTrusted = app.clockHealthOwner != nullptr &&
      app.clockHealthOwner->clockTrusted();
  const att::HState clockHealth = app.health.state();
  const bool clockHealthAllowed = clockHealth == att::H_OK ||
      clockHealth == att::H_CLOCK_FAULT || clockHealth == att::H_SENSOR_FAULT;
  const bool clockJournalReady = app.eventGroupAuditReady && app.log != nullptr &&
      !app.log->faulted() && app.eventGroupAudit.healthy();
  const bool clockEnabled = app.clockConfirmationRoutesReady && clockJournalReady &&
      app.clockWritesAuthorized && app.storageOwner != nullptr &&
      app.storageOwner->writable() && clockHealthAllowed && !app.faulted;
  const uint16_t* pendingClockId = app.eventGroupAudit.pendingClockRequestId();
  const uint16_t* completedClockId = app.eventGroupAudit.completedClockRequestId();
  char pendingClockIdText[13] = {};
  char completedClockIdText[13] = {};
  if (app.eventGroupAudit.clockRequestPending() && pendingClockId != nullptr) {
    snprintf(pendingClockIdText, sizeof pendingClockIdText, "%04x%04x%04x",
             static_cast<unsigned>(pendingClockId[0]),
             static_cast<unsigned>(pendingClockId[1]),
             static_cast<unsigned>(pendingClockId[2]));
  }
  if (app.eventGroupAudit.lastClockRequestCompleted() && completedClockId != nullptr) {
    snprintf(completedClockIdText, sizeof completedClockIdText, "%04x%04x%04x",
             static_cast<unsigned>(completedClockId[0]),
             static_cast<unsigned>(completedClockId[1]),
             static_cast<unsigned>(completedClockId[2]));
  }
  const bool rosterMutationEnabled =
      app.adminRosterHttpHandler != nullptr && app.rosterWriter != nullptr &&
      !app.rosterWriter->faulted() && app.clockWritesAuthorized &&
      app.storageOwner != nullptr && app.storageOwner->writable() &&
      app.clockHealthOwner != nullptr && app.clockHealthOwner->clockTrusted() &&
      (app.health.state() == att::H_OK || app.health.state() == att::H_SENSOR_FAULT) &&
      app.log != nullptr && !app.log->faulted() && app.roster != nullptr &&
      app.roster->historyComplete() && !app.faulted;
  const bool manualAttendanceEnabled =
      app.adminRosterHttpHandler != nullptr && app.attendanceWriter != nullptr &&
      !app.attendanceWriter->faulted() && app.clockWritesAuthorized &&
      app.storageOwner != nullptr && app.storageOwner->writable() &&
      app.clockHealthOwner != nullptr && app.clockHealthOwner->clockTrusted() &&
      (app.health.state() == att::H_OK || app.health.state() == att::H_SENSOR_FAULT) &&
      app.log != nullptr && !app.log->faulted() && app.roster != nullptr &&
      app.roster->historyComplete() && !app.faulted;
  const att::Config* statusConfig = app.configBoot.config();
  const char* closureBootReconciliation = "not_started";
  if (app.closureCoordinatorFaulted ||
      (app.closureCoordinator != nullptr && app.closureCoordinator->faulted())) {
    closureBootReconciliation = "faulted";
  } else if (app.closureCoordinator != nullptr &&
             app.closureCoordinator->state() == att::CCT_PAUSED) {
    closureBootReconciliation = "paused";
  } else if (app.closureCoordinatorStarted &&
             app.closureCoordinator != nullptr &&
             app.closureCoordinator->state() == att::CCT_ACTIVE) {
    closureBootReconciliation = "complete";
  }
  const bool attendanceEnabled = attendanceRuntimeReady(app);
  bool scanWindowOpen = false;
  if (attendanceEnabled && statusConfig != nullptr && clockTrusted) {
    att::Ds3231Reading currentTime = {0, false};
    if (app.rtcClock.readUtcEpoch(0, currentTime) == att::DS3231_OK &&
        !currentTime.oscillatorStopped && currentTime.epochUtc >= att::EPOCH_2020) {
      const int activeSession = att::sessionAt(
          statusConfig->tt, statusConfig->params, currentTime.epochUtc);
      scanWindowOpen = activeSession == 0 || activeSession == 1;
    }
  }
  const bool closureSchedulerEnabled = app.closureCoordinatorStarted &&
      app.closureCoordinator != nullptr && app.closureCoordinator->active();
  const bool enrollmentActive = app.as608AttendanceOwner != nullptr &&
      app.as608AttendanceOwner->enrollmentActive();
  const att::EnrollState enrollmentState =
      app.as608AttendanceOwner != nullptr && app.as608AttendanceOwner->startupReady()
          ? app.as608AttendanceOwner->enrollmentState() : att::EN_IDLE;
  char payload[2000] = {};
  const int length = snprintf(
      payload, sizeof payload,
      "{\"boot\":\"ready\",\"ap\":\"ready\","
      "\"storage\":\"layout_validated\",\"log_replay\":\"%s\","
      "\"log_records\":%lu,\"log_baseline_utc\":%lu,\"event_group_audit\":\"%s\",\"roster_history\":\"%s\",\"attendance_admission\":\"%s\","
      "\"sensor_startup\":\"%s\",\"sensor_capacity\":%u,\"closure_pending\":%s,"
      "\"closure_boot_reconciliation\":\"%s\",\"closure_scheduler_enabled\":%s,"
      "\"rtc_observation\":\"%s\",\"rtc_candidate_utc\":%lu,"
      "\"i2c_bus_ready\":%s,\"i2c_recovery_attempts\":%lu,"
      "\"health\":\"%s\",\"clock_trusted\":%s,"
      "\"clock_journal_ready\":%s,\"clock_request_pending\":%s,"
      "\"clock_pending_request_id\":\"%s\","
      "\"clock_pending_requested_utc\":%lu,"
      "\"clock_pending_clock_pair_committed\":%s,"
      "\"clock_last_completed_request_id\":\"%s\","
      "\"clock_last_completed_requested_utc\":%lu,"
      "\"clock_confirmation_enabled\":%s,\"clock_write_authorized\":%s,"
      "\"roster_mutation_enabled\":%s,\"manual_attendance_enabled\":%s,\"utc_offset_min\":%d,"
      "\"attendance_enabled\":%s,\"scan_window_open\":%s,"
      "\"admin_routes_enabled\":%s,"
      "\"enrollment_active\":%s,\"enrollment_state\":\"%s\","
      "\"sensor_index_valid\":%s,\"sensor_templates\":%u,"
      "\"last_scan_result\":\"%s\",\"last_scan_student_id\":%u,"
      "\"last_scan_epoch\":%lu,"
      "\"admin_auth_routes_enabled\":%s,"
      "\"maintenance_state\":\"%s\",\"maintenance_error\":\"%s\","
      "\"config_provisioning_enabled\":false,"
      "\"setup_code_remaining_seconds\":0,\"sf2_export_enabled\":false}",
      app.logReplayReady ? "ok" : "fault",
      static_cast<unsigned long>(app.logRecordCount),
      static_cast<unsigned long>(app.logBaselineEpoch),
      app.eventGroupAuditReady ? "ok" : "fault",
      app.rosterHistoryReady ? "ok" : "fault",
      app.attendanceAdmissionAttached ? "attached" : "fault",
      sensorStartupText(app),
      static_cast<unsigned>(app.sensorStartupReady ? app.sensorCapacity : 0),
      (app.closureReady && app.closure != nullptr && app.closure->pending())
          ? "true" : "false",
      closureBootReconciliation,
      closureSchedulerEnabled ? "true" : "false",
      clockTrusted ? "confirmed" : rtcStartupObservationText(app.rtcObservation),
      static_cast<unsigned long>(clockTrusted ? app.logBaselineEpoch : app.rtcEpochUtc),
      app.i2cBus.ready() ? "true" : "false",
      static_cast<unsigned long>(app.i2cRecoveryAttempts),
      healthStateText(app.health.state()),
      clockTrusted ? "true" : "false",
      clockJournalReady ? "true" : "false",
      app.eventGroupAudit.clockRequestPending() ? "true" : "false",
      pendingClockIdText,
      static_cast<unsigned long>(app.eventGroupAudit.clockRequestPending()
          ? app.eventGroupAudit.pendingClockRequestEpoch() : 0),
      app.eventGroupAudit.pendingClockPairCommitted() ? "true" : "false",
      completedClockIdText,
      static_cast<unsigned long>(app.eventGroupAudit.lastClockRequestCompleted()
          ? app.eventGroupAudit.completedClockRequestEpoch() : 0),
      clockEnabled ? "true" : "false",
      (app.clockWritesAuthorized && app.storageOwner != nullptr &&
       app.storageOwner->writable()) ? "true" : "false",
      rosterMutationEnabled ? "true" : "false",
      manualAttendanceEnabled ? "true" : "false",
      statusConfig != nullptr ? static_cast<int>(statusConfig->params.utcOffsetMin) : 0,
      attendanceEnabled ? "true" : "false",
      scanWindowOpen ? "true" : "false",
      app.adminRosterHttpHandler != nullptr ? "true" : "false",
      enrollmentActive ? "true" : "false",
      enrollStateText(enrollmentState),
      app.sensorOccupancy.valid ? "true" : "false",
      static_cast<unsigned>(app.sensorOccupancy.valid ? app.sensorOccupancy.count() : 0),
      app.lastScanResult != nullptr ? app.lastScanResult : "waiting",
      static_cast<unsigned>(app.lastScanStudentId),
      static_cast<unsigned long>(app.lastScanEpoch),
      app.adminAuthRoutesReady ? "true" : "false",
      app.maintenanceAction != ADMIN_MAINTENANCE_NONE ? "busy" :
          (app.maintenanceError != nullptr && strcmp(app.maintenanceError, "none") != 0
              ? "failed" : "idle"),
      app.maintenanceError != nullptr ? app.maintenanceError : "unknown");
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    app.http.send(500, "application/json", "{}");
    return;
  }
  app.http.send(200, "application/json", payload);
}

void sendRecoveryStatus(Application& app) {
  const bool recoveryActive = app.recoveryMode &&
      app.recoveryService != nullptr && app.recoveryService->active() &&
      !app.recoveryService->cleanupPending() && app.recoveryPresence.asserted();
  const bool recoveryRoutes = recoveryActive &&
      app.recoveryHttpHandler != nullptr && app.adminAuthRoutesReady;
  char payload[384] = {};
  const int length = snprintf(
      payload, sizeof payload,
      "{\"boot\":\"recovery_mode\",\"ap\":\"ready\","
      "\"storage\":\"recovery_only\",\"health\":\"%s\","
      "\"recovery_mode\":true,\"recovery_active\":%s,"
      "\"recovery_routes_enabled\":%s,"
      "\"admin_auth_routes_enabled\":%s,"
      "\"recovery_requires_restart\":true,"
      "\"attendance_enabled\":false,\"clock_trusted\":false}",
      healthStateText(app.health.state()),
      recoveryActive ? "true" : "false",
      recoveryRoutes ? "true" : "false",
      app.adminAuthRoutesReady ? "true" : "false");
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    app.http.send(500, "application/json", "{}");
    return;
  }
  app.http.send(200, "application/json", payload);
}

void addReadOnlyHeaders(WebServer& server) {
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader(
      "Content-Security-Policy",
      "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
      "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
}

const char* enrollStateText(att::EnrollState state) {
  switch (state) {
    case att::EN_IDLE: return "idle";
    case att::EN_WAIT_FIRST_FINGER: return "place_first_finger";
    case att::EN_CONVERT_FIRST: return "reading_first_sample";
    case att::EN_SEARCH_DUPLICATE: return "checking_duplicate";
    case att::EN_DUPLICATE_FOUND: return "duplicate_found";
    case att::EN_WAIT_SECOND_FINGER: return "remove_then_place_again";
    case att::EN_CONVERT_SECOND: return "reading_second_sample";
    case att::EN_CREATE_MODEL: return "building_template";
    case att::EN_STORE_MODEL: return "saving_template";
    case att::EN_WAIT_FINGER_REMOVAL: return "remove_finger";
    case att::EN_WAIT_VERIFY_FINGER: return "verify_finger";
    case att::EN_CONVERT_VERIFY: return "checking_verification";
    case att::EN_SEARCH_VERIFY: return "searching_verification";
    case att::EN_VERIFY_FAILED: return "verification_failed";
    case att::EN_RETRY_REQUIRED: return "retry_required";
    case att::EN_SLOT_TAKEN: return "sensor_slot_taken";
    case att::EN_ROSTER_CHANGED: return "roster_changed";
    case att::EN_SENSOR_FAULT: return "sensor_fault";
    case att::EN_OCCUPANCY_REFRESH_REQUIRED: return "refreshing_sensor_index";
    case att::EN_DONE: return "complete";
    case att::EN_CANCELLED: return "cancelled";
    case att::EN_CANCEL_PENDING_RELEASE: return "cancelling_release_finger";
    case att::EN_CANCEL_RELEASE_FAULT: return "cancel_release_fault";
    default: return "unknown";
  }
}

bool decodeAdminBearer(const String& header, uint8_t token[16]) {
  static const char prefix[] = "Bearer ";
  if (token == nullptr || header.length() != 39 ||
      header.substring(0, 7) != prefix) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char hi = header[7 + i * 2];
    const char lo = header[8 + i * 2];
    const int high = hi >= '0' && hi <= '9' ? hi - '0' :
        hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 :
        hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
    const int low = lo >= '0' && lo <= '9' ? lo - '0' :
        lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 :
        lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
    if (high < 0 || low < 0) return false;
    token[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool authorizeEnrollmentRequest(Application& app, att::Ep endpoint,
                                uint8_t token[16], att::ApiResult& result) {
  const bool decoded = decodeAdminBearer(app.http.header("Authorization"), token);
  if (app.adminSessions == nullptr || !decoded) {
    memset(token, 0, 16);
    result = att::ApiResult{att::A_UNAUTH, att::R_NO_TOKEN, 0};
    return false;
  }
  result = app.adminSessions->authorize(endpoint, app.health.state(), token,
                                         monotonicSeconds());
  memset(token, 0, 16);
  return result.code == att::A_OK;
}

void sendEnrollmentStatus(Application& app, int statusCode = 200) {
  const att::As608AttendanceOwner* owner = app.as608AttendanceOwner;
  const bool available = owner != nullptr && owner->startupReady();
  const bool active = available && owner->enrollmentActive();
  const att::EnrollState state = available ? owner->enrollmentState() : att::EN_IDLE;
  char payload[512] = {};
  const int length = snprintf(payload, sizeof payload,
      "{\"available\":%s,\"active\":%s,\"faulted\":%s,"
      "\"state\":\"%s\",\"current_id\":%u,\"duplicate_id\":%u,"
      "\"duplicate_score\":%u,\"queue_size\":%u,\"queue_position\":%u,"
      "\"enrolled\":%u,\"verified\":%u,\"verification_failed\":%u,"
      "\"skipped\":%u,\"verify_after_enroll\":%s,"
      "\"occupancy_valid\":%s,\"template_count\":%u}",
      available ? "true" : "false", active ? "true" : "false",
      available && owner->enrollmentFaulted() ? "true" : "false",
      enrollStateText(state),
      static_cast<unsigned>(available ? owner->enrollmentCurrentId() : 0),
      static_cast<unsigned>(available ? owner->enrollmentDuplicateId() : 0),
      static_cast<unsigned>(available ? owner->enrollmentDuplicateScore() : 0),
      static_cast<unsigned>(available ? owner->enrollmentQueueSize() : 0),
      static_cast<unsigned>(available ? owner->enrollmentQueuePosition() : 0),
      static_cast<unsigned>(available ? owner->enrollmentCount() : 0),
      static_cast<unsigned>(available ? owner->enrollmentVerifiedCount() : 0),
      static_cast<unsigned>(available ? owner->enrollmentVerificationFailedCount() : 0),
      static_cast<unsigned>(available ? owner->enrollmentSkippedCount() : 0),
      available && owner->enrollmentVerificationEnabled() ? "true" : "false",
      app.sensorOccupancy.valid ? "true" : "false",
      static_cast<unsigned>(app.sensorOccupancy.valid ? app.sensorOccupancy.count() : 0));
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    app.http.send(500, "application/json", "{}");
    return;
  }
  app.http.send(statusCode, "application/json", payload);
}

bool enrollmentRequestIsBodyless(WebServer& server) {
  const String contentLength = server.header("Content-Length");
  return (contentLength.length() == 0 || contentLength == "0") &&
      server.header("Content-Type").length() == 0 &&
      server.header("Transfer-Encoding").length() == 0;
}

bool eraseNvsNamespaceAndVerify(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  nvs_handle_t handle;
  esp_err_t error = nvs_open(name, NVS_READWRITE, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return true;
  if (error != ESP_OK) return false;
  error = nvs_erase_all(handle);
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) return false;

  // A successful commit is not enough: reopen the exact namespace and verify
  // that no application-owned keys remain. Never erase the NVS partition.
  error = nvs_open(name, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return true;
  if (error != ESP_OK) return false;
  size_t usedEntries = 1;
  error = nvs_get_used_entry_count(handle, &usedEntries);
  nvs_close(handle);
  return error == ESP_OK && usedEntries == 0;
}

bool clearApplicationNvsForFactoryReset() {
  // Keep attcfg last. If any earlier namespace cannot be verified empty, the
  // existing admin PIN/config remains available for supervised recovery.
  static const char* const namespaces[] = {
      "attrstx", "attclos", "attwifi", "attsetup", "attsec", "attcfg"};
  for (size_t i = 0; i < sizeof namespaces / sizeof namespaces[0]; ++i) {
    if (!eraseNvsNamespaceAndVerify(namespaces[i])) return false;
  }
  return true;
}

bool attendanceWindowCurrentlyOpen(Application& app) {
  const att::Config* config = app.configBoot.config();
  if (config == nullptr || app.clockHealthOwner == nullptr ||
      !app.clockHealthOwner->clockTrusted()) return true;
  att::Ds3231Reading reading = {0, false};
  if (app.rtcClock.readUtcEpoch(0, reading) != att::DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < att::EPOCH_2020)
    return true;
  const int activeSession = att::sessionAt(
      config->tt, config->params, reading.epochUtc);
  return activeSession >= 0;
}

void handleEnrollmentAction(Application& app, const char* action) {
  uint8_t token[16] = {};
  att::ApiResult authorization = {att::A_UNAUTH, att::R_NO_TOKEN, 0};
  if (!authorizeEnrollmentRequest(app, att::EP_ENROLL, token, authorization)) {
    app.http.send(static_cast<int>(authorization.code), "application/json",
                  "{\"error\":\"enrollment_not_authorized\"}");
    return;
  }
  if (!enrollmentRequestIsBodyless(app.http) || app.http.args() != 0) {
    app.http.send(400, "application/json", "{\"error\":\"unexpected_enrollment_request_data\"}");
    return;
  }
  att::As608AttendanceOwner* owner = app.as608AttendanceOwner;
  if (owner == nullptr || !owner->startupReady()) {
    app.http.send(423, "application/json", "{\"error\":\"sensor_not_ready\"}");
    return;
  }
  bool accepted = false;
  if (strcmp(action, "cancel") == 0) {
    accepted = owner->enrollmentActive();
    if (accepted) owner->cancelEnrollment();
  } else if (strcmp(action, "retry") == 0) {
    accepted = owner->retryEnrollment();
  } else if (strcmp(action, "different") == 0) {
    accepted = owner->tryDifferentEnrollmentFinger();
  } else if (strcmp(action, "skip") == 0) {
    accepted = owner->skipEnrollmentStudent();
  } else if (strcmp(action, "retry-verification") == 0) {
    accepted = owner->retryEnrollmentVerification();
  } else if (strcmp(action, "continue") == 0) {
    accepted = owner->continueAfterEnrollmentVerificationFailure();
  } else if (strcmp(action, "retry-cancel-release") == 0) {
    accepted = owner->retryEnrollmentCancellation();
  }
  sendEnrollmentStatus(app, accepted ? 202 : 409);
}

bool attendanceRuntimeReady(const Application& app) {
  return app.bootState == att::CFG_BOOT_READY && !app.recoveryMode &&
      !app.faulted && app.sensorStartupReady && !app.sensorStartupFaulted &&
      app.sensorOccupancy.valid && app.as608AttendanceOwner != nullptr &&
      app.as608AttendanceOwner->startupReady() &&
      !app.as608AttendanceOwner->attendanceFaulted() &&
      !app.as608AttendanceOwner->transportFaulted() &&
      !app.as608AttendanceOwner->enrollmentActive() &&
      app.closureCoordinatorStarted && app.closureCoordinator != nullptr &&
      app.closureCoordinator->active() && !app.closureCoordinatorFaulted &&
      app.attendanceGate != nullptr && app.attendanceGate->readyForAttendance() &&
      app.clockHealthOwner != nullptr && app.clockHealthOwner->clockTrusted() &&
      app.storageOwner != nullptr && app.storageOwner->writable() &&
      app.log != nullptr && app.log->readyToAppend();
}

void registerHttpRoutes(Application& app) {
  app.http.on("/", HTTP_GET, [&app]() {
    addReadOnlyHeaders(app.http);
    // The dashboard is embedded as a pre-compressed gzip byte stream to keep
    // the firmware inside the 2.5 MiB factory-app partition. Every supported
    // browser used to access this local dashboard advertises gzip support.
    app.http.sendHeader("Content-Encoding", "gzip");
    app.http.sendHeader("Vary", "Accept-Encoding");
    app.http.send_P(200, "text/html; charset=utf-8",
                    reinterpret_cast<const char*>(kDashboardGzip),
                    sizeof(kDashboardGzip));
  });

  app.http.on("/api/status", HTTP_GET, [&app]() {
    addReadOnlyHeaders(app.http);
    if (app.recoveryMode) {
      sendRecoveryStatus(app);
    } else if (app.bootState == att::CFG_BOOT_NEEDS_SETUP) {
      sendNeedsSetupStatus(app);
    } else {
      sendConfiguredStatus(app);
    }
  });

  const bool setupRouteReady =
      app.bootState == att::CFG_BOOT_NEEDS_SETUP &&
      app.runtimeRandom.rfStarted() &&
      app.firstRunSequence != nullptr && !app.firstRunSequence->faulted() &&
      app.firstRunSetupBoundary != nullptr &&
      !app.firstRunSetupBoundary->faulted() &&
      app.setupCodeAuthRuntime.ready() && !app.setupCodeVerifier.faulted();

  const bool authRouteReady =
      app.bootState == att::CFG_BOOT_READY &&
      app.runtimeRandom.rfStarted() &&
      app.pinAuthRuntime.ready() &&
      app.adminSessions != nullptr && !app.adminSessions->faulted() &&
      app.adminPinWriter != nullptr && app.adminPinWriter->ready() &&
      app.adminAuthHttpHandler != nullptr;

  const bool clockRouteReady = !app.recoveryMode && authRouteReady &&
      app.clockWritesAuthorized && app.clockHealthOwner != nullptr &&
      app.log != nullptr && app.months != nullptr && app.storageOwner != nullptr &&
      app.storageOwner->writable();
  if (clockRouteReady && app.adminClockHttpHandler == nullptr) {
    app.adminClockHttpHandler = new (app.adminClockHttpHandlerMemory)
        att::Esp32AdminClockHttpHandler(
            *app.clockHealthOwner, *app.log, *app.months, monotonicSeconds,
            currentClockHealthInputs, app.faulted);
  }
  const bool clockHandlerReady = clockRouteReady &&
      app.adminClockHttpHandler != nullptr;

  const bool recoveryRouteReady = app.recoveryMode && authRouteReady &&
      app.recoveryService != nullptr && app.recoveryService->active() &&
      app.recoveryPresenceReady && app.recoveryPresence.asserted() &&
      !app.recoveryService->cleanupPending();
  if (recoveryRouteReady && app.recoveryHttpHandler == nullptr) {
    app.recoveryHttpHandler = new (app.recoveryHttpHandlerMemory)
        att::Esp32RecoveryHttpHandler(
            *app.recoveryService, *app.adminSessions, app.recoveryPresence,
            monotonicSeconds);
  }
  const bool recoveryHandlerReady = recoveryRouteReady &&
      app.recoveryHttpHandler != nullptr;

  const bool maintenanceRouteReady = authRouteReady && !app.recoveryMode &&
      app.bootState == att::CFG_BOOT_READY;
  if (maintenanceRouteReady) {
    app.http.on("/api/v1/admin/restart", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http);
      uint8_t token[16] = {};
      att::ApiResult authorization = {att::A_UNAUTH, att::R_NO_TOKEN, 0};
      if (!authorizeEnrollmentRequest(app, att::EP_CONFIG_GET, token, authorization)) {
        app.http.send(static_cast<int>(authorization.code), "application/json",
                      "{\"error\":\"admin_not_authorized\"}");
        return;
      }
      if (!enrollmentRequestIsBodyless(app.http) || app.http.args() != 0) {
        app.http.send(400, "application/json", "{\"error\":\"unexpected_restart_request_data\"}");
        return;
      }
      if (app.maintenanceAction != ADMIN_MAINTENANCE_NONE) {
        app.http.send(409, "application/json", "{\"error\":\"maintenance_already_pending\"}");
        return;
      }
      app.maintenanceError = "none";
      app.maintenanceAction = ADMIN_MAINTENANCE_RESTART;
      app.http.send(202, "application/json", "{\"accepted\":true,\"action\":\"restart\"}");
    });

    app.http.on("/api/v1/admin/reset", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http);
      uint8_t token[16] = {};
      att::ApiResult authorization = {att::A_UNAUTH, att::R_NO_TOKEN, 0};
      if (!authorizeEnrollmentRequest(app, att::EP_ROSTER_SET, token, authorization)) {
        app.http.send(static_cast<int>(authorization.code), "application/json",
                      "{\"error\":\"admin_not_authorized\"}");
        return;
      }
      if (!enrollmentRequestIsBodyless(app.http) || app.http.args() != 1 ||
          app.http.argName(0) != "mode" || !app.http.hasArg("mode")) {
        app.http.send(400, "application/json", "{\"error\":\"reset_mode_required\"}");
        return;
      }
      const String mode = app.http.arg("mode");
      const bool factory = mode == "factory";
      if (!factory && mode != "records") {
        app.http.send(400, "application/json", "{\"error\":\"invalid_reset_mode\"}");
        return;
      }
      if (app.maintenanceAction != ADMIN_MAINTENANCE_NONE) {
        app.http.send(409, "application/json", "{\"error\":\"maintenance_already_pending\"}");
        return;
      }
      const bool storageReady = app.storageOwner != nullptr &&
          app.storageOwner->writable() && app.log != nullptr &&
          app.log->readyToAppend() && app.attendanceGate != nullptr &&
          app.attendanceGate->readyForAttendance();
      const bool sensorReady = app.as608AttendanceOwner != nullptr &&
          app.sensorStartupReady && !app.sensorStartupFaulted &&
          app.sensorOccupancy.valid && app.as608AttendanceOwner->startupReady() &&
          !app.as608AttendanceOwner->attendanceFaulted() &&
          !app.as608AttendanceOwner->enrollmentFaulted() &&
          !app.as608AttendanceOwner->transportFaulted() &&
          !app.as608AttendanceOwner->scanActive() &&
          !app.as608AttendanceOwner->enrollmentActive();
      const bool closureReady = app.closureReady &&
          app.closureCoordinatorStarted && app.closureCoordinator != nullptr &&
          app.closureCoordinator->active() && !app.closureCoordinatorFaulted;
      const bool clockTransactionIdle =
          !app.eventGroupAudit.clockRequestPending();
      if (app.faulted || app.health.state() != att::H_OK || !storageReady ||
          !sensorReady || !closureReady || !clockTransactionIdle ||
          (app.closure != nullptr && app.closure->pending()) ||
          attendanceWindowCurrentlyOpen(app)) {
        app.http.send(423, "application/json", "{\"error\":\"reset_blocked_by_device_health_or_active_class\"}");
        return;
      }
      if (!app.as608AttendanceOwner->beginClearAllTemplates()) {
        if (app.as608AttendanceOwner->clearTemplatesStatus() ==
                att::AS608_CLEAR_FAILED ||
            app.as608AttendanceOwner->enrollmentFaulted()) {
          app.sensorStartupFaulted = true;
          app.maintenanceError = "fingerprint_clear_start_failed_data_preserved";
          updateRuntimeHealth(app);
          showHealthStatusWhenChanged(app);
        }
        app.http.send(409, "application/json",
                      "{\"error\":\"fingerprint_sensor_busy_or_clear_failed\"}");
        return;
      }
      app.maintenanceError = "none";
      app.maintenanceAction = factory ? ADMIN_MAINTENANCE_FACTORY_RESET
                                      : ADMIN_MAINTENANCE_CLEAR_RECORDS;
      app.http.send(202, "application/json", factory
          ? "{\"accepted\":true,\"action\":\"factory_reset\"}"
          : "{\"accepted\":true,\"action\":\"clear_records_and_fingerprints\"}");
    });
  }

  // The pinned parser patch enforces these framing headers before it accepts
  // any raw request body. Collect once for every registered mutation handler.
  if (setupRouteReady || authRouteReady || clockHandlerReady || recoveryHandlerReady) {
    const char* requestHeaders[] = {
        "Content-Type", "Content-Length", "Transfer-Encoding",
        "Authorization"};
    app.http.collectHeaders(requestHeaders,
                            sizeof requestHeaders / sizeof requestHeaders[0]);
  }

  if (authRouteReady && app.as608AttendanceOwner != nullptr) {
    app.http.on("/api/v1/enrollment", HTTP_GET, [&app]() {
      addReadOnlyHeaders(app.http);
      uint8_t token[16] = {};
      att::ApiResult authorization = {att::A_UNAUTH, att::R_NO_TOKEN, 0};
      if (!authorizeEnrollmentRequest(app, att::EP_ROSTER_GET, token, authorization)) {
        app.http.send(static_cast<int>(authorization.code), "application/json",
                      "{\"error\":\"enrollment_status_not_authorized\"}");
        return;
      }
      if (!enrollmentRequestIsBodyless(app.http) || app.http.args() != 0) {
        app.http.send(400, "application/json", "{\"error\":\"unexpected_enrollment_request_data\"}");
        return;
      }
      sendEnrollmentStatus(app);
    });
    app.http.on("/api/v1/enrollment/start", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http);
      uint8_t token[16] = {};
      att::ApiResult authorization = {att::A_UNAUTH, att::R_NO_TOKEN, 0};
      if (!authorizeEnrollmentRequest(app, att::EP_ENROLL, token, authorization)) {
        app.http.send(static_cast<int>(authorization.code), "application/json",
                      "{\"error\":\"enrollment_not_authorized\"}");
        return;
      }
      if (!enrollmentRequestIsBodyless(app.http) || app.http.args() > 1 ||
          (app.http.args() == 1 && app.http.argName(0) != "verify")) {
        app.http.send(400, "application/json", "{\"error\":\"unexpected_enrollment_request_data\"}");
        return;
      }
      if (app.as608AttendanceOwner == nullptr ||
          !app.as608AttendanceOwner->startupReady()) {
        app.http.send(423, "application/json", "{\"error\":\"sensor_not_ready\"}");
        return;
      }
      bool verify = true;
      if (app.http.hasArg("verify")) {
        const String choice = app.http.arg("verify");
        if (choice == "0") verify = false;
        else if (choice != "1") {
          app.http.send(400, "application/json", "{\"error\":\"invalid_verify_choice\"}");
          return;
        }
      }
      const att::EnrollStart result = app.as608AttendanceOwner->beginEnrollment(verify);
      if (result == att::ES_BEGIN_OK) sendEnrollmentStatus(app, 202);
      else if (result == att::ES_BEGIN_NO_WORK) sendEnrollmentStatus(app, 200);
      else {
        const int code = result == att::ES_BEGIN_BUSY ? 409 : 423;
        sendEnrollmentStatus(app, code);
      }
    });
    app.http.on("/api/v1/enrollment/cancel", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "cancel");
    });
    app.http.on("/api/v1/enrollment/retry", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "retry");
    });
    app.http.on("/api/v1/enrollment/different", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "different");
    });
    app.http.on("/api/v1/enrollment/skip", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "skip");
    });
    app.http.on("/api/v1/enrollment/retry-verification", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "retry-verification");
    });
    app.http.on("/api/v1/enrollment/continue", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "continue");
    });
    app.http.on("/api/v1/enrollment/retry-cancel-release", HTTP_POST, [&app]() {
      addReadOnlyHeaders(app.http); handleEnrollmentAction(app, "retry-cancel-release");
    });
  }

  if (setupRouteReady) {
    app.firstRunHttpHandler = new (app.firstRunHttpHandlerMemory)
        att::Esp32FirstRunHttpHandler(*app.firstRunSetupBoundary, app.health,
                                      monotonicSeconds, app.faulted);
    app.http.addHandler(app.firstRunHttpHandler);
    app.firstRunSetupRouteReady = true;
  }

  // READY auth routes expose login/logout/current-PIN change, roster administration,
  // operator corrections, authenticated maintenance and the dedicated enrollment owner.
  // Automatic fingerprint attendance is served by the serialized application loop.
  if (authRouteReady) {
    app.http.addHandler(app.adminAuthHttpHandler);
    app.adminAuthRoutesReady = true;
    const att::Config* readyConfig = app.configBoot.config();
    if (!app.recoveryMode && app.log != nullptr && app.months != nullptr &&
        app.logReplayReady && app.eventGroupAuditReady && app.roster != nullptr &&
        app.rosterHistoryReady && readyConfig != nullptr) {
      if (app.clockHealthOwner != nullptr && app.rosterWriter == nullptr) {
        app.rosterWriter = new (app.rosterWriterMemory)
            att::RosterEventWriter(*app.log, *app.roster);
      }
      if (app.attendanceAdmissionAttached && app.closureReady &&
          app.eventGroupAuditReady && app.rosterHistoryReady &&
          app.attendanceWriter == nullptr) {
        att::LogObserver* requiredAttendanceObservers[] = {
            &app.eventGroupAudit, app.roster, app.attendanceAdmission};
        app.attendanceWriter = new (app.attendanceWriterMemory)
            att::AttendanceEventWriter(*app.log, app.eventGroupAudit,
                requiredAttendanceObservers, 3);
      }
      app.adminRosterHttpHandler = new (app.adminRosterHttpHandlerMemory)
          att::Esp32AdminRosterHttpHandler(
              *app.adminSessions, *app.log, *app.months, *app.roster, *readyConfig,
              app.health, monotonicSeconds, app.faulted, app.rosterWriter,
              &app.rtcClock, app.clockHealthOwner, &app.clockWritesAuthorized,
              app.attendanceWriter, app.closureReady ? &app.closure->index() : nullptr);
      app.http.addHandler(app.adminRosterHttpHandler);
    }
  }

  // Clock correction is a separately gated READY domain mutation alongside
  // authenticated roster and manual-attendance administration. It requires a request-bound ADMIN
  // bearer token and the exact storage/replay gate; the serialized loop is its
  // sole writer task. It remains the only domain write allowed while clock
  // trust is faulted.
  if (clockHandlerReady) {
    app.http.addHandler(app.adminClockHttpHandler);
    app.clockConfirmationRoutesReady = true;
  }

  // Recovery is a separate maintenance-only HTTP composition. The handler
  // has no attendance, roster, closure, enrollment or normal storage APIs.
  if (recoveryHandlerReady) app.http.addHandler(app.recoveryHttpHandler);

  app.http.onNotFound([&app]() {
    addReadOnlyHeaders(app.http);
    if (app.http.method() != HTTP_GET) {
      app.http.send(405, "text/plain; charset=utf-8", "Method not allowed");
    } else {
      app.http.send(404, "text/plain; charset=utf-8", "Not found");
    }
  });

  app.http.begin();
  app.httpReady = true;
}

bool startStorage(Application& app) {
  const att::Config* config = app.configBoot.config();
  if (app.storageReady && app.storageOwner != nullptr &&
      app.months != nullptr && app.log != nullptr && app.logReplayReady &&
      app.eventGroupAuditReady && app.rosterHistoryReady &&
      app.attendanceAdmissionAttached && app.closureCheckpointLoaded)
    return true;
  if (config == nullptr || app.storageOwner != nullptr ||
      app.months != nullptr || app.log != nullptr) return false;

  // A provisioned Config is not sufficient to start an attendance runtime:
  // the separately committed first-run closure checkpoint must exist, decode,
  // and bind to this exact immutable go-live day before filesystem startup.
  att::ClosureCheckpoint checkpoint = {};
  const att::ClosureCheckpointStoreCode checkpointCode =
      app.closureCheckpointStore.load(checkpoint);
  if (checkpointCode != att::CCS_OK ||
      checkpoint.goLiveDay != config->goLiveDay) {
    app.closureCheckpointFaulted = true;
    return false;
  }
  app.closureCheckpointLoaded = true;

  // A full-chip erase leaves the signed data partition entirely erased.
  // Initialize only that provably blank partition; never format on a mount
  // failure and never reformat any partition containing even one non-FF byte.
  // NEEDS_SETUP never reaches startStorage(), so formatting happens only after
  // first-run configuration has been committed and the device reboots READY.
  if (!app.storageInitBackend.initializeBlankPartitionIfErased())
    return false;

  // Placement construction defers the timezone-dependent owner until after
  // validated config boot. The ESP VFS mount is write-capable, but the owner
  // exposes a read-only serialized file set until the application replay gate
  // explicitly authorizes the clock-audit writer.
  app.storageOwner = new (app.storageOwnerMemory) att::LittleFsStorageOwner(
      app.storageMountBackend, kLittleFsMountPoint,
      att::kSignedLittleFsPartitionLabel, config->params.utcOffsetMin);
  // Bind recovery to this exact normal owner. Healthy boots leave the service
  // inactive; only a later, validated storage-fault path may hand off ownership.
  app.recoveryService = new (app.recoveryServiceMemory)
      att::Esp32RecoveryService(
          *app.storageOwner, kLittleFsMountPoint,
          att::kSignedLittleFsPartitionLabel, config->params.utcOffsetMin,
          app.runtimeRandom);
  if (app.oledReady)
    (void)app.oled.showStorageProgress(att::EOSP_MOUNTING_READ_ONLY);
  if (!app.storageOwner->begin()) return false;

  att::IFileSet* files = app.storageOwner->fileSet();
  if (app.storageOwner->state() != att::LITTLEFS_OWNER_READY || !files)
    return false;

  // MonthSet/LogStore and every projection are replayed while the serialized
  // file set is still read-only. The write gate opens only after the clock
  // service/auth owners exist and the exact replay graph is accepted.
  app.months = new (app.monthSetMemory) att::MonthSet(*files);
  app.log = new (app.logStoreMemory) att::LogStore(*app.months);
  if (app.oledReady)
    (void)app.oled.showStorageProgress(att::EOSP_VALIDATING_LOG);
  const att::OpenInfo opened = app.log->open();
  if (!opened.ok || app.log->faulted()) return false;
  app.logReplayReady = true;
  app.logRecordCount = opened.records;
  app.logBaselineEpoch = opened.baselineEpoch;

  // Build all independent projections before replaying the prefix. Grouped
  // attachment preserves each observer's log order but reads each record once.
  // Do not run RosterTransitionAudit here: E27 is a monthly-reporting guard,
  // not a reason to reject operational same-day lifecycle history.
  app.roster = new (app.rosterMemory) att::RosterStore(config->params.utcOffsetMin);
  app.attendanceAdmission = new (app.attendanceAdmissionMemory)
      att::LogAttendanceAdmission(*app.log, config->params, false);
  if (app.oledReady)
    (void)app.oled.showStorageProgress(att::EOSP_REPLAYING_PROJECTIONS);
  att::LogObserver* bootObservers[] = {
      &app.eventGroupAudit, app.roster, app.attendanceAdmission};
  if (!app.log->attachMany(bootObservers, 3) ||
      !app.log->hasObserver(&app.eventGroupAudit) ||
      !app.log->hasObserver(app.roster) ||
      !app.log->hasObserver(app.attendanceAdmission)) return false;
  if (!app.eventGroupAudit.readyFor(*app.log, opened) ||
      !app.roster->historyComplete() ||
      !app.attendanceAdmission->attached() ||
      !app.attendanceAdmission->usesLog(*app.log) ||
      !app.attendanceAdmission->usesParams(config->params)) return false;
  app.eventGroupAuditReady = true;
  app.rosterHistoryReady = true;
  app.attendanceAdmissionAttached = true;

  // ClosureEngine validates finalized session coverage against the roster during
  // historical replay, then filters live expected students through the shared AS608
  // occupancy projection. Runtime closure is held until a complete sensor index exists.
  // Configure the closure index from a current, OSF-clear RTC sample before
  // attaching it: historical replay must discard expired CLOSE/GAP facts first.
  att::Ds3231Reading closureClock = {0, false};
  if (app.rtcClock.readUtcEpoch(0, closureClock) != att::DS3231_OK ||
      closureClock.oscillatorStopped || closureClock.epochUtc < att::EPOCH_2020)
    return false;
  const int32_t closureToday =
      att::dayNumber(closureClock.epochUtc, config->params.utcOffsetMin);
  if (closureToday < config->goLiveDay ||
      closureToday > att::MAX_SUPPORTED_DAY) return false;
  app.sensorAwareRoster = new (app.sensorAwareRosterMemory)
      att::SensorAwareRoster(*app.roster, app.sensorOccupancy);
  app.closure = new (app.closureMemory) att::ClosureEngine(
      *app.log, config->params, config->tt, *app.sensorAwareRoster,
      att::MAX_ID, config->goLiveDay, closureToday);
  if (!app.closure->ready()) return false;
  // The durable checkpoint is loaded and reconciled later, only after the AS608
  // occupancy snapshot is complete. This avoids treating unenrolled active students
  // as attendance-eligible during live closure.
  app.closureReady = true;

  app.attendanceGate = new (app.attendanceGateMemory)
      att::ApplicationAttendanceGate(
          app.configBoot, *app.storageOwner, *app.months, *app.log,
          *app.roster, *app.sensorAwareRoster, *app.attendanceAdmission,
          *app.closure, app.health);
  app.storageReady = true;
  return true;
}

att::Ds3231Code readRuntimeRtcSample(Application& app,
                                      att::Ds3231Reading& reading) {
  if (!app.loopRtcCacheEnabled) {
    return app.i2cBus.ready()
        ? app.rtcClock.readUtcEpoch(0, reading) : att::DS3231_IO_ERROR;
  }
  if (!app.loopRtcReadAttempted) {
    app.loopRtcReadAttempted = true;
    app.loopRtcReading = att::Ds3231Reading{0, false};
    app.loopRtcReadCode = app.i2cBus.ready()
        ? app.rtcClock.readUtcEpoch(0, app.loopRtcReading)
        : att::DS3231_IO_ERROR;
  }
  reading = app.loopRtcReading;
  return app.loopRtcReadCode;
}

bool currentClockHealthInputs(Application& app, att::HealthInputs& inputs) {
  if (app.bootState != att::CFG_BOOT_READY || !app.configBoot.ready())
    return false;

  att::Ds3231Reading reading = {0, false};
  const att::Ds3231Code readCode = readRuntimeRtcSample(app, reading);

  const bool calendarValid = readCode == att::DS3231_OK;
  const bool rtcAnswered = calendarValid || readCode == att::DS3231_INVALID_TIME;
  const bool belowFloor = !calendarValid || reading.epochUtc < att::EPOCH_2020;
  const bool belowLog = calendarValid && app.logReplayReady &&
      app.logBaselineEpoch != 0 && reading.epochUtc < app.logBaselineEpoch;
  const bool storageOk = app.storageReady && app.logReplayReady &&
      app.storageOwner != nullptr &&
      app.storageOwner->state() == att::LITTLEFS_OWNER_READY &&
      app.storageOwner->fileSet() != nullptr && app.log != nullptr &&
      !app.log->faulted() && app.eventGroupAuditReady &&
      app.rosterHistoryReady && app.attendanceAdmissionAttached &&
      app.closureReady && app.closure != nullptr;
  const bool sensorOk = app.sensorStartupReady && !app.sensorStartupFaulted;

  app.rtcPresent = rtcAnswered;
  app.rtcCalendarValid = calendarValid;
  app.rtcLostPower = calendarValid && reading.oscillatorStopped;
  app.rtcEpochUtc = calendarValid ? reading.epochUtc : 0;
  if (!app.i2cBus.ready()) app.rtcObservation = RTC_STARTUP_BUS_UNAVAILABLE;
  else if (readCode == att::DS3231_IO_ERROR)
    app.rtcObservation = RTC_STARTUP_READ_FAILED;
  else if (readCode == att::DS3231_INVALID_TIME)
    app.rtcObservation = RTC_STARTUP_INVALID_CALENDAR;
  else if (reading.oscillatorStopped)
    app.rtcObservation = RTC_STARTUP_OSF_SET;
  else if (belowFloor)
    app.rtcObservation = RTC_STARTUP_BELOW_FLOOR;
  else if (belowLog)
    app.rtcObservation = RTC_STARTUP_BELOW_LOG;
  else if (app.clockHealthOwner != nullptr &&
           app.clockHealthOwner->clockTrusted())
    app.rtcObservation = RTC_STARTUP_CONFIRMED;
  else
    app.rtcObservation = RTC_STARTUP_CANDIDATE_UNTRUSTED;

  att::HealthInputs candidate = {
      true, rtcAnswered, calendarValid && reading.oscillatorStopped,
      belowFloor, belowLog, storageOk, true, sensorOk, false, false};

  // A completed clock transaction is a durable operator-acceptance record.
  // On MCU reboot, the RTC may remain powered and continue counting; do not
  // force another manual clock set when the DS3231 OSF is clear and its live
  // epoch is not behind either that accepted epoch or the replayed log. The
  // owner independently refuses this path for pending/malformed audit state.
  if (readCode == att::DS3231_OK && app.clockHealthOwner != nullptr &&
      !app.clockHealthOwner->clockTrusted() && app.eventGroupAuditReady &&
      app.log != nullptr && !app.log->faulted()) {
    const bool restored =
        app.clockHealthOwner->restoreFromCompletedJournal(
            candidate, app.eventGroupAudit.healthy(),
            app.eventGroupAudit.clockRequestPending(),
            app.eventGroupAudit.lastClockRequestCompleted(),
            app.eventGroupAudit.completedClockRequestEpoch(),
            reading.epochUtc, app.logBaselineEpoch);
    if (restored) app.rtcObservation = RTC_STARTUP_CONFIRMED;
  }

  inputs = candidate;
  return true;
}

bool currentClockHealthInputs(att::HealthInputs& inputs) {
  return gApp != nullptr && currentClockHealthInputs(*gApp, inputs);
}

void updateRuntimeHealth(Application& app) {
  if (app.clockHealthOwner != nullptr) {
    att::HealthInputs current = {};
    if (currentClockHealthInputs(app, current))
      (void)app.clockHealthOwner->update(current);
    else {
      const att::HealthInputs failed = {
          true, false, false, true, false, false, false,
          false, false, false};
      (void)app.clockHealthOwner->update(failed);
    }
    return;
  }

  const bool setupRequired = app.bootState == att::CFG_BOOT_NEEDS_SETUP;
  const bool storageOk = setupRequired ||
      (app.storageReady && app.logReplayReady && app.log != nullptr &&
       app.eventGroupAuditReady && app.rosterHistoryReady &&
       app.attendanceAdmissionAttached && app.closureReady &&
       app.closure != nullptr && app.roster != nullptr &&
       !app.log->faulted());
  const bool epochBelowLog = app.rtcCalendarValid && app.logReplayReady &&
      app.logBaselineEpoch != 0 && app.rtcEpochUtc < app.logBaselineEpoch;
  const bool epochBelowFloor = !app.rtcCalendarValid ||
      app.rtcEpochUtc < att::EPOCH_2020;

  att::HealthInputs inputs = {
      true, app.rtcPresent, app.rtcLostPower, epochBelowFloor, epochBelowLog,
      storageOk, true, app.sensorStartupReady && !app.sensorStartupFaulted,
      false, false};
  (void)att::updateHealthFromConfigBoot(app.health, app.configBoot, inputs);

  if (epochBelowLog && !app.rtcLostPower)
    app.rtcObservation = RTC_STARTUP_BELOW_LOG;
}

void showHealthStatusWhenChanged(Application& app) {
  if (!app.oledReady) return;
  const uint8_t mask = app.health.mask();
  if (app.oledHealthMask == mask) return;
  const att::Esp32OledActionCode result = app.oled.showHealthStatus(
      (mask & att::HB_CLOCK) == 0, (mask & att::HB_SENSOR) == 0);
  // A failed lock acquisition must remain eligible for retry on the next
  // bounded health poll; only suppress redraw after a transfer was requested.
  if (result == att::EOA_TRANSFER_REQUESTED) app.oledHealthMask = mask;
}

uint16_t closureHealthGapCause(const Application& app, bool rtcTrusted) {
  if (!rtcTrusted || (app.health.mask() & att::HB_CLOCK) != 0)
    return att::GAP_CLOCK;
  if ((app.health.mask() & att::HB_STORAGE) != 0 || app.log == nullptr ||
      app.log->faulted() || app.storageOwner == nullptr ||
      !app.storageOwner->writable()) return att::GAP_STORAGE;
  if ((app.health.mask() & att::HB_SENSOR) != 0 ||
      !app.sensorStartupReady || app.sensorStartupFaulted)
    return att::GAP_SENSOR;
  return att::GAP_POWER;
}

bool closureHealthIsGood(const Application& app, bool rtcTrusted) {
  return rtcTrusted && app.health.mask() == 0 &&
      app.clockWritesAuthorized && app.storageReady && app.log != nullptr &&
      app.log->readyToAppend() && app.roster != nullptr &&
      app.roster->historyComplete() && app.sensorStartupReady &&
      !app.sensorStartupFaulted && app.sensorOccupancy.valid;
}

// Compose boot/outage reconciliation and health transitions. Runtime start is held
// until the complete sensor index is available; the coordinator's separate periodic
// service then runs only while the clock, storage, sensor and roster gates are sound.
void serviceClosureCheckpointLifecycle(Application& app) {
  if (app.bootState != att::CFG_BOOT_READY || app.recoveryMode || app.faulted ||
      app.closureCoordinatorFaulted || app.closure == nullptr ||
      app.log == nullptr || app.roster == nullptr ||
      app.clockHealthOwner == nullptr || app.storageOwner == nullptr ||
      !app.sensorStartupReady || app.sensorStartupFaulted ||
      !app.sensorOccupancy.valid) return;

  att::Ds3231Reading reading = {0, false};
  const att::Ds3231Code readCode = readRuntimeRtcSample(app, reading);
  const bool rtcTrusted = readCode == att::DS3231_OK &&
      !reading.oscillatorStopped && reading.epochUtc >= att::EPOCH_2020 &&
      app.clockHealthOwner->clockTrusted();
  uint32_t observationEpoch = rtcTrusted ? reading.epochUtc :
      app.closureLastHealthyEpoch;

  if (!app.closureCoordinatorStartAttempted) {
    // Never initialize or infer a checkpoint here. First-run provisioning owns
    // create-only initialization; READY boot must load that exact record.
    if (!rtcTrusted || !app.clockWritesAuthorized ||
        !app.storageOwner->writable() || !app.log->readyToAppend() ||
        !app.closureCheckpointLoaded) return;
    const att::Config* config = app.configBoot.config();
    if (config == nullptr) return;
    const int32_t today = att::dayNumber(
        reading.epochUtc, config->params.utcOffsetMin);
    if (today < config->goLiveDay || today > att::MAX_SUPPORTED_DAY) return;

    app.closureCoordinator = new (app.closureCoordinatorMemory)
        att::ClosureCheckpointCoordinator(
            *app.closure, *app.log, app.closureCheckpointStore,
            config->goLiveDay, config->params.utcOffsetMin);
    app.closureCoordinatorStartAttempted = true;
    const att::ClosureCoordinatorCode startCode =
        app.closureCoordinator->start(reading.epochUtc);
    if (startCode != att::CCC_OK && startCode != att::CCC_TIME_ROLLBACK) {
      app.closureCoordinatorFaulted = true;
      return;
    }
    app.closureCoordinatorStarted = true;
    app.closureLastHealthyEpoch = reading.epochUtc;

    if (startCode == att::CCC_TIME_ROLLBACK) {
      app.closureHealthObservationKnown = true;
      app.closureHealthHealthy = false;
      return;
    }

    const bool healthy = closureHealthIsGood(app, rtcTrusted);
    if (healthy) {
      app.closureHealthObservationKnown = true;
      app.closureHealthHealthy = true;
      return;
    }
    const att::ClosureCoordinatorCode healthCode =
        app.closureCoordinator->healthChanged(
            observationEpoch, false, closureHealthGapCause(app, rtcTrusted));
    if (healthCode != att::CCC_PAUSED_RESULT && healthCode != att::CCC_OK) {
      app.closureCoordinatorFaulted = true;
      return;
    }
    app.closureHealthObservationKnown = true;
    app.closureHealthHealthy = false;
    return;
  }

  if (!app.closureCoordinatorStarted || app.closureCoordinator == nullptr)
    return;
  if (app.closureCoordinator->faulted()) {
    app.closureCoordinatorFaulted = true;
    return;
  }
  if (observationEpoch == 0) return;

  const bool healthy = closureHealthIsGood(app, rtcTrusted);
  if (app.closureHealthObservationKnown &&
      app.closureHealthHealthy == healthy) {
    // Advance the recovery boundary only while every required subsystem is
    // healthy. On the first unhealthy observation, use this prior sample so
    // the GAP conservatively includes the interval since health was last known
    // good, rather than starting at the poll that detected the failure.
    if (healthy && rtcTrusted)
      app.closureLastHealthyEpoch = reading.epochUtc;
    return;
  }
  if (!app.closureHealthObservationKnown && !healthy) {
    // A startup rollback already has a PAUSED engine. Keep the initial unhealthy
    // observation latched; a later trusted timestamp must prove positive recovery.
    app.closureHealthObservationKnown = true;
    app.closureHealthHealthy = false;
    return;
  }
  if (!healthy && app.closureHealthHealthy &&
      app.closureLastHealthyEpoch != 0)
    observationEpoch = app.closureLastHealthyEpoch;
  else if (healthy && rtcTrusted)
    observationEpoch = reading.epochUtc;

  const att::ClosureCoordinatorCode healthCode =
      app.closureCoordinator->healthChanged(
          observationEpoch, healthy,
          healthy ? static_cast<uint16_t>(att::GAP_POWER)
                  : closureHealthGapCause(app, rtcTrusted));
  if (healthCode == att::CCC_PAUSED_RESULT && healthy) {
    // Equal-second or still-rolled-back recovery is not enough. Retry on the
    // next health poll; the engine requires a positive, monotonic GAP interval.
    app.closureHealthObservationKnown = true;
    app.closureHealthHealthy = false;
    return;
  }
  if (healthCode != att::CCC_OK && healthCode != att::CCC_PAUSED_RESULT) {
    app.closureCoordinatorFaulted = true;
    return;
  }
  app.closureHealthObservationKnown = true;
  app.closureHealthHealthy = healthy && healthCode == att::CCC_OK;
  if (app.closureHealthHealthy && rtcTrusted)
    app.closureLastHealthyEpoch = reading.epochUtc;
}

void serviceRuntimeClockHealth(Application& app) {
  if (app.bootState != att::CFG_BOOT_READY || app.recoveryMode ||
      app.faulted || app.clockHealthOwner == nullptr) return;
  const int64_t nowUs = esp_timer_get_time();
  if (nowUs < 0 || nowUs < app.nextRtcHealthPollUs) return;
  app.nextRtcHealthPollUs = nowUs + kRtcHealthPollPeriodUs;
  updateRuntimeHealth(app);

  // A missing RTC can leave the ESP32 controller in a stale driver state
  // after pins are disturbed. Require two consecutive transport failures
  // before restarting the shared bus; do not attempt recovery for invalid
  // calendar data or the DS3231 oscillator-stop flag. The owner serializes
  // the restart against both RTC and OLED clients.
  const bool rtcTransportFailed =
      app.rtcObservation == RTC_STARTUP_READ_FAILED;
  if (!app.i2cRecoveryTerminalFault &&
      app.i2cRecoveryPolicy.observeRtcTransportResult(
          rtcTransportFailed, nowUs)) {
    if (app.i2cRecoveryAttempts < UINT32_MAX) ++app.i2cRecoveryAttempts;
    const bool recovered = app.i2cBus.recover();
    app.loopRtcReadAttempted = false;
    if (!recovered) {
      app.i2cRecoveryTerminalFault = true;
      app.rtcObservation = RTC_STARTUP_BUS_UNAVAILABLE;
    }
    // Force a complete health redraw after the shared controller restart.
    app.oledHealthMask = 0xFFu;
  }
  showHealthStatusWhenChanged(app);
  serviceClosureCheckpointLifecycle(app);
}

bool startClockConfirmation(Application& app) {
  if (app.clockConfirmationService != nullptr ||
      !app.storageReady ||
      !app.i2cBus.ready() || app.adminSessions == nullptr ||
      app.storageOwner == nullptr || app.months == nullptr || app.log == nullptr)
    return false;

  app.clockConfirmationService = new (app.clockConfirmationServiceMemory)
      att::ClockConfirmationService(
          app.rtcClock, app.monotonicClock, app.health, app.configBoot,
          *app.storageOwner);
  app.adminClockConfirmation = new (app.adminClockConfirmationMemory)
      att::AdminClockConfirmation(
          *app.adminSessions, *app.clockConfirmationService);
  app.clockHealthOwner = new (app.clockHealthOwnerMemory)
      att::ClockHealthOwner(*app.adminClockConfirmation, app.health,
                            app.configBoot);
  if (!app.clockHealthOwner->ready()) {
    app.clockHealthOwner = nullptr;
    app.adminClockConfirmation = nullptr;
    app.clockConfirmationService = nullptr;
    return false;
  }
  return true;
}

const char* sensorStartupText(const Application& app) {
  if (app.sensorStartupFaulted ||
      (app.as608AttendanceOwner != nullptr &&
       app.as608AttendanceOwner->startupFaulted())) return "fault";
  if (app.sensorStartupReady && app.as608AttendanceOwner != nullptr &&
      app.as608AttendanceOwner->startupReady()) return "ready";
  if (app.sensorStartupStarted) return "pending";
  return "not_started";
}

uint8_t currentBuzzerSoundMode(const Application& app) {
  if (app.recoveryMode || app.bootState != att::CFG_BOOT_READY)
    return att::BUZZER_SOUND_SILENT;
  const att::Config* config = app.configBoot.config();
  return config != nullptr
      ? static_cast<uint8_t>(config->soundMode)
      : static_cast<uint8_t>(att::BUZZER_SOUND_SILENT);
}

void playBuzzer(Application& app, att::BuzzerPattern pattern) {
  gBuzzer.play(pattern, currentBuzzerSoundMode(app), millis());
}

bool isEnrollmentFingerPrompt(att::EnrollState state) {
  return state == att::EN_WAIT_FIRST_FINGER ||
      state == att::EN_WAIT_SECOND_FINGER ||
      state == att::EN_WAIT_FINGER_REMOVAL ||
      state == att::EN_WAIT_VERIFY_FINGER;
}

void observeEnrollmentBuzzerState(Application& app) {
  if (app.as608AttendanceOwner == nullptr) return;
  const att::EnrollState state = app.as608AttendanceOwner->enrollmentState();
  if (!app.enrollmentSoundStateKnown) {
    app.lastEnrollmentSoundState = state;
    app.enrollmentSoundStateKnown = true;
    return;
  }
  if (state == app.lastEnrollmentSoundState) return;
  app.lastEnrollmentSoundState = state;
  if (isEnrollmentFingerPrompt(state)) {
    playBuzzer(app, att::BUZZER_PATTERN_ENROLLMENT_STEP);
  } else if (state == att::EN_SENSOR_FAULT ||
             state == att::EN_CANCEL_RELEASE_FAULT) {
    playBuzzer(app, att::BUZZER_PATTERN_FAULT);
  }
}

void playAttendanceBuzzer(Application& app,
                          att::AttendancePipelineResult result) {
  switch (result) {
    case att::ATT_PIPE_SAVED_PRESENT:
      playBuzzer(app, att::BUZZER_PATTERN_PRESENT);
      break;
    case att::ATT_PIPE_SAVED_LATE:
      playBuzzer(app, att::BUZZER_PATTERN_LATE);
      break;
    case att::ATT_PIPE_ALREADY_RECORDED:
      playBuzzer(app, att::BUZZER_PATTERN_ALREADY_RECORDED);
      break;
    case att::ATT_PIPE_BAD_IMAGE:
      playBuzzer(app, att::BUZZER_PATTERN_BAD_IMAGE);
      break;
    case att::ATT_PIPE_NO_MATCH:
      playBuzzer(app, att::BUZZER_PATTERN_NO_MATCH);
      break;
    case att::ATT_PIPE_OUTSIDE_WINDOW:
    case att::ATT_PIPE_REJECTED:
      playBuzzer(app, att::BUZZER_PATTERN_OUTSIDE_WINDOW);
      break;
    case att::ATT_PIPE_NOT_ACTIVE:
      playBuzzer(app, att::BUZZER_PATTERN_NOT_ACTIVE);
      break;
    case att::ATT_PIPE_NOT_SAVED:
      playBuzzer(app, att::BUZZER_PATTERN_NOT_SAVED);
      break;
    case att::ATT_PIPE_SENSOR_FAULT:
    case att::ATT_PIPE_COMMIT_UNCERTAIN:
      playBuzzer(app, att::BUZZER_PATTERN_FAULT);
      break;
    case att::ATT_PIPE_IDLE:
    case att::ATT_PIPE_PENDING:
    case att::ATT_PIPE_NO_FINGER:
    default:
      // No-finger polling and incomplete transactions are not user-visible outcomes.
      break;
  }
}

bool startAs608Runtime(Application& app) {
  const att::Config* config = app.configBoot.config();
  if (app.bootState != att::CFG_BOOT_READY || config == nullptr ||
      !app.i2cBus.ready() || !app.storageReady || !app.closureReady ||
      app.storageOwner == nullptr || app.months == nullptr ||
      app.log == nullptr || app.roster == nullptr ||
      app.attendanceAdmission == nullptr || app.closure == nullptr ||
      app.attendanceGate == nullptr || !app.attendanceGate->readyForSensorStartup())
    return false;

  // Authenticate and read the complete sensor index. The configured password is
  // verified only; the module password is never changed. GPIO16/17 and 57600 baud
  // are centralized here so hardware pin/baud changes do not leak into the pipeline.
  Serial2.begin(kAs608Baud, SERIAL_8N1, kAs608RxPin, kAs608TxPin);
  app.as608AttendanceOwner = new (app.as608AttendanceOwnerMemory)
      att::As608AttendanceOwner(
          Serial2, *app.log, *app.roster, app.sensorOccupancy, *config,
          app.as608PageResolver, *app.attendanceGate, kAs608FactoryPassword);
  if (!app.as608AttendanceOwner->beginStartup()) {
    app.sensorStartupFaulted = true;
    playBuzzer(app, att::BUZZER_PATTERN_FAULT);
    return false;
  }
  app.sensorStartupStarted = true;
  return true;
}

void serviceAs608Runtime(Application& app) {
  if (!app.sensorStartupStarted || app.sensorStartupFaulted ||
      app.as608AttendanceOwner == nullptr) return;

  if (app.sensorStartupReady) {
    // Catch state changes made by HTTP actions since the previous loop pass,
    // then service exactly one bounded sensor/enrollment step and observe again.
    observeEnrollmentBuzzerState(app);
    app.as608AttendanceOwner->service();
    observeEnrollmentBuzzerState(app);
    return;
  }

  const att::As608StartupStatus status =
      app.as608AttendanceOwner->stepStartup();
  if (status == att::AS608_STARTUP_READY &&
      app.as608AttendanceOwner->startupReady()) {
    app.sensorStartupReady = true;
    app.scanPromptVisible = false;
    app.sensorCapacity = app.as608AttendanceOwner->sensorCapacity();
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
    if (app.oledReady)
      (void)app.oled.showAttendanceMessage("WAITING", "Wait for class to start");
    app.lastScanResult = "waiting_for_class";
  } else if (status == att::AS608_STARTUP_FAULT ||
             app.as608AttendanceOwner->startupFaulted() ||
             app.as608AttendanceOwner->transportFaulted()) {
    app.sensorStartupFaulted = true;
    playBuzzer(app, att::BUZZER_PATTERN_FAULT);
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
  }
}

// Idle OLED: alternate the status frame with the AP address so an operator can
// find the dashboard without a README. The Wi-Fi password is never shown here.
// Only called after fault, setup and scan-feedback owners have declined the panel.
void showIdleFrame(Application& app, int64_t nowUs) {
  if (!app.oledReady) return;
  char address[16] = {};
  const bool haveAddress = app.wifiAp.getAddress(address, sizeof address);
  att::OledIdleFrame frame = att::OIF_STATUS;
  if (!app.idleCarousel.poll(nowUs, haveAddress, frame)) return;
  if (frame == att::OIF_ADDRESS) {
    char url[40] = {};
    snprintf(url, sizeof url, "http://%s/", address);
    char detail[80] = {};
    snprintf(detail, sizeof detail, "Open in browser:\n%s\n(ignore no internet)", url);
    if (app.oled.showAttendanceMessage("WI-FI: att", detail) !=
        att::EOA_TRANSFER_REQUESTED)
      app.idleCarousel.reset();  // redraw on the next poll
  } else if (app.oled.showAttendanceMessage("WAITING", "Wait for class to start") !=
             att::EOA_TRANSFER_REQUESTED) {
    app.idleCarousel.reset();
  }
}

void setScanFeedback(Application& app, const char* result,
                     const char* title, const char* detail,
                     uint16_t studentId = 0, uint32_t scanEpoch = 0) {
  app.lastScanResult = result;
  app.lastScanStudentId = studentId;
  app.lastScanEpoch = scanEpoch;
  app.scanFeedbackUntilUs = esp_timer_get_time() + 2200000LL;
  app.scanPromptVisible = false;
  app.idleCarousel.reset();
  if (app.oledReady) (void)app.oled.showAttendanceMessage(title, detail);
}

bool oledWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void advanceUtf8Codepoint(const char*& text) {
  if (text == nullptr || *text == '\0') return;
  const uint8_t lead = static_cast<uint8_t>(*text);
  size_t width = 1;
  if ((lead & 0xE0u) == 0xC0u) width = 2;
  else if ((lead & 0xF0u) == 0xE0u) width = 3;
  else if ((lead & 0xF8u) == 0xF0u) width = 4;
  size_t bytes = 1;
  while (bytes < width && text[bytes] != '\0') ++bytes;
  text += bytes;
}

char oledInitial(const char*& text) {
  if (text == nullptr || *text == '\0') return '\0';
  const uint8_t value = static_cast<uint8_t>(*text);
  advanceUtf8Codepoint(text);
  // The built-in OLED font is ASCII. Preserve ASCII initials; render a safe
  // placeholder for a non-ASCII first codepoint rather than leaking a name.
  if (value >= 'a' && value <= 'z') return static_cast<char>(value - 'a' + 'A');
  if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))
    return static_cast<char>(value);
  return '?';
}

void makeOledStudentLabel(const char* name, uint16_t studentId,
                          char* output, size_t outputSize) {
  if (output == nullptr || outputSize == 0) return;
  output[0] = '\0';
  const char* cursor = name != nullptr ? name : "";
  while (oledWhitespace(*cursor)) ++cursor;
  const char first = oledInitial(cursor);
  if (first == '\0') {
    snprintf(output, outputSize, "ID %u", static_cast<unsigned>(studentId));
    return;
  }

  const char* secondStart = cursor;
  const char* firstWordEnd = cursor;
  while (*firstWordEnd != '\0' && !oledWhitespace(*firstWordEnd))
    advanceUtf8Codepoint(firstWordEnd);
  const bool hasNextWord = *firstWordEnd != '\0';
  if (hasNextWord) {
    secondStart = firstWordEnd;
    while (oledWhitespace(*secondStart)) ++secondStart;
  }
  if (*secondStart == '\0') secondStart = cursor; // one-word name: use its next initial
  const char second = oledInitial(secondStart);
  if (second == '\0')
    snprintf(output, outputSize, "ID %u %c", static_cast<unsigned>(studentId), first);
  else
    snprintf(output, outputSize, "ID %u %c%c", static_cast<unsigned>(studentId),
             first, second);
}

void serviceAutomaticNoClassDetection(Application& app, uint32_t trustedNow) {
  if (app.closure == nullptr || !app.closure->ready() || app.log == nullptr ||
      app.roster == nullptr || app.storageOwner == nullptr ||
      !app.storageOwner->writable() || !app.log->readyToAppend() ||
      app.log->faulted() || !app.roster->historyComplete() ||
      !app.roster->healthy() || app.clockHealthOwner == nullptr ||
      !app.clockHealthOwner->clockTrusted() || !app.sensorStartupReady ||
      app.sensorStartupFaulted || !app.sensorOccupancy.valid ||
      app.as608AttendanceOwner == nullptr ||
      app.as608AttendanceOwner->enrollmentActive() ||
      app.as608AttendanceOwner->scanActive() || app.health.state() != att::H_OK)
    return;

  const att::Config* config = app.configBoot.config();
  if (config == nullptr || !att::validateConfig(*config)) return;

  // LogIndex is a bounded replay projection of raw immutable events. An
  // overflow/malformed projection disables automation rather than inferring
  // zero evidence from an incomplete index.
  const att::LogIndex& index = app.closure->index();
  if (!index.evidenceHealthy() || index.evidenceOverflow() ||
      index.pendingAutoNoClassCount() >= att::MAX_AUTO_NOCLASS_PENDING)
    return;  // Never append an alert that cannot fit in the durable RAM projection.

  att::SessionEvidence candidate = {};
  bool foundCandidate = false;
  // Evidence is sorted and bounded to the active 400-day window. Skip stale
  // evidence for sessions that are no longer enabled in the current timetable;
  // one old candidate must never starve later valid sessions.
  for (size_t i = 0; i < index.evidenceCount(); ++i) {
    if (!index.sessionEvidenceAt(i, candidate)) return;
    if (!candidate.closed || !candidate.coverageComplete ||
        candidate.expectedCount == 0 || candidate.acceptedScans != 0 ||
        candidate.manualAttendanceEntries != 0 || candidate.manualNoClass ||
        candidate.autoEvaluated || candidate.session > 1 ||
        candidate.day < config->goLiveDay ||
        candidate.day > att::MAX_SUPPORTED_DAY) continue;
    const uint8_t weekday = att::weekdayIndex(candidate.day);
    if (weekday > 4 || !config->tt.day[weekday][candidate.session].enabled)
      continue;
    const att::Slot& slot = config->tt.day[weekday][candidate.session];
    if (slot.start >= slot.end || slot.end > 1439 ||
        slot.start < config->params.scanLeadMin) continue;
    const uint64_t candidateCloseEpoch = static_cast<uint64_t>(
        att::dayStartEpoch(candidate.day, config->params.utcOffsetMin)) +
        static_cast<uint64_t>(slot.end) * 60u;
    // Evidence is ordered by day/session; once the first otherwise eligible
    // candidate is in the future, every later candidate is also in the future.
    if (candidateCloseEpoch > trustedNow) return;
    foundCandidate = true;
    break;
  }
  if (!foundCandidate) return;

  // This is deliberately labelled as a system inference, not proof of a
  // cancelled class. It is appended only after EV_CLOSE exists with complete
  // coverage, a non-empty expected roster, and zero raw scan/manual evidence.
  const size_t beforeCount = app.log->count();
  const uint32_t beforeSeq = app.log->lastSeq();
  const uint64_t generation = app.log->openGeneration();
  if (beforeCount > static_cast<size_t>(-1) - 1u ||
      beforeSeq >= UINT32_MAX - 1u || generation == 0) return;
  const att::Event automatic = {
      att::EV_AUTO_NOCLASS_SET,
      static_cast<uint8_t>(candidate.session == 0 ? 0u : att::FL_SESSION),
      candidate.expectedCount, trustedNow, 0,
      static_cast<uint16_t>(candidate.day)};
  if (!app.log->append(&automatic, 1)) {
    app.faulted = true;
    app.closureCoordinatorFaulted = true;
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
    return;
  }
  att::Event stored = {};
  const bool readback = app.log->at(beforeCount, stored) &&
      stored.seq == beforeSeq + 1u &&
      stored.type == att::EV_AUTO_NOCLASS_SET &&
      stored.flags == automatic.flags && stored.a == automatic.a &&
      stored.epoch == automatic.epoch && stored.b == automatic.b &&
      app.log->openGeneration() == generation &&
      app.log->count() == beforeCount + 1u &&
      app.log->lastSeq() == beforeSeq + 1u &&
      !app.log->faulted() && index.evidenceHealthy();
  if (!readback) {
    app.faulted = true;
    app.closureCoordinatorFaulted = true;
    app.log->requireReopen();
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
  }
}

void serviceClosureScheduler(Application& app) {
  if (app.bootState != att::CFG_BOOT_READY || app.recoveryMode || app.faulted ||
      app.closureCoordinatorFaulted || !app.closureCoordinatorStarted ||
      app.closureCoordinator == nullptr || !app.closureCoordinator->active() ||
      !app.sensorStartupReady || app.sensorStartupFaulted ||
      !app.sensorOccupancy.valid || app.as608AttendanceOwner == nullptr ||
      app.as608AttendanceOwner->enrollmentActive() ||
      app.as608AttendanceOwner->scanActive() || app.clockHealthOwner == nullptr ||
      !app.clockHealthOwner->clockTrusted() || app.storageOwner == nullptr ||
      !app.storageOwner->writable() || app.log == nullptr ||
      !app.log->readyToAppend() || app.health.state() != att::H_OK) return;

  const int64_t nowUs = esp_timer_get_time();
  if (nowUs < 0 || nowUs < app.nextClosureServiceUs) return;
  app.nextClosureServiceUs = nowUs + 250000LL;

  att::Ds3231Reading reading = {0, false};
  if (readRuntimeRtcSample(app, reading) != att::DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < att::EPOCH_2020) return;
  const att::ClosureCoordinatorCode code =
      app.closureCoordinator->service(reading.epochUtc);
  // Re-check already-closed sessions on every non-faulted coordinator tick.
  // A transient sensor/health/storage gate may have suppressed automation on
  // the first close tick; once the gate recovers, the durable closure remains
  // eligible for evaluation without requiring another closure transition.
  if (code == att::CCC_OK || code == att::CCC_NO_TICK ||
      code == att::CCC_STEP_PENDING) {
    serviceAutomaticNoClassDetection(app, reading.epochUtc);
    return;
  }

  app.closureCoordinatorFaulted = true;
  updateRuntimeHealth(app);
  showHealthStatusWhenChanged(app);
}

void serviceAutomaticAttendance(Application& app) {
  if (app.as608AttendanceOwner == nullptr || !app.sensorStartupReady ||
      app.sensorStartupFaulted || app.recoveryMode || app.faulted) return;
  att::As608AttendanceOwner& owner = *app.as608AttendanceOwner;
  if (owner.enrollmentActive()) return; // owner.service() exclusively drives the enrollment FSM

  if (owner.attendanceFaulted() || owner.transportFaulted()) {
    app.sensorStartupFaulted = true;
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
    return;
  }

  if (owner.scanActive()) {
    const att::AttendancePipelineResult result = owner.stepScan();
    if (result == att::ATT_PIPE_PENDING) return;
    if (result == att::ATT_PIPE_NO_FINGER || result == att::ATT_PIPE_IDLE) {
      // These results end the one-shot capture transaction. Keep the READY
      // prompt visible, but release the UI latch so a fresh capture can start.
      app.scanPromptVisible = false;
      app.lastScanResult = "ready";
      app.nextAttendanceScanUs = esp_timer_get_time() + 150000LL;
      return;
    }

    const uint16_t id = owner.lastScanStudentId();
    char name[att::ROSTER_NAME_MAX + 1] = {};
    if (id != 0 && app.roster != nullptr)
      (void)app.roster->nameOf(id, name);
    char oledStudentLabel[24] = {};
    if (id != 0)
      makeOledStudentLabel(name, id, oledStudentLabel, sizeof oledStudentLabel);
    if (result == att::ATT_PIPE_SAVED_PRESENT ||
        result == att::ATT_PIPE_SAVED_LATE) {
      const bool late = result == att::ATT_PIPE_SAVED_LATE;
      setScanFeedback(app, late ? "late" : "present",
                      late ? "LATE" : "PRESENT",
                      oledStudentLabel[0] != '\0' ? oledStudentLabel : "Attendance saved", id,
                      owner.lastScanEpoch());
    } else if (result == att::ATT_PIPE_ALREADY_RECORDED) {
      setScanFeedback(app, "already_recorded", "ALREADY", "Already recorded", id,
                      owner.lastScanEpoch());
    } else if (result == att::ATT_PIPE_NO_MATCH) {
      setScanFeedback(app, "no_match", "NOT FOUND", "Retry or ask admin", 0,
                      owner.lastScanEpoch());
    } else if (result == att::ATT_PIPE_BAD_IMAGE) {
      setScanFeedback(app, "bad_image", "TRY AGAIN", "Place finger flat", 0,
                      owner.lastScanEpoch());
    } else if (result == att::ATT_PIPE_NOT_ACTIVE) {
      setScanFeedback(app, "not_active", "NOT ENROLLED", "Ask admin to enroll", 0,
                      owner.lastScanEpoch());
    } else if (result == att::ATT_PIPE_SENSOR_FAULT) {
      setScanFeedback(app, "sensor_fault", "SENSOR FAULT", "Attendance paused", 0,
                      owner.lastScanEpoch());
      app.sensorStartupFaulted = true;
      updateRuntimeHealth(app);
      showHealthStatusWhenChanged(app);
    } else if (result == att::ATT_PIPE_NOT_SAVED) {
      // No event append was attempted. Do not mislabel a storage/admission gate
      // failure as a dead sensor; the runtime readiness gate will keep scans paused.
      setScanFeedback(app, "not_saved", "NOT SAVED", "Attendance paused", 0,
                      owner.lastScanEpoch());
      updateRuntimeHealth(app);
      showHealthStatusWhenChanged(app);
    } else if (result == att::ATT_PIPE_COMMIT_UNCERTAIN) {
      setScanFeedback(app, "save_uncertain", "SAVE ERROR", "Do not rescan", id,
                      owner.lastScanEpoch());
      updateRuntimeHealth(app);
      showHealthStatusWhenChanged(app);
    } else if (result == att::ATT_PIPE_REJECTED ||
               result == att::ATT_PIPE_OUTSIDE_WINDOW) {
      setScanFeedback(app, "paused", "NOT RECORDED", "Check class status", 0,
                      owner.lastScanEpoch());
    }
    playAttendanceBuzzer(app, result);
    app.nextAttendanceScanUs = esp_timer_get_time() + 350000LL;
    return;
  }

  if (!attendanceRuntimeReady(app)) return;
  const int64_t nowUs = esp_timer_get_time();
  if (nowUs < 0 || nowUs < app.nextAttendanceScanUs) return;
  app.nextAttendanceScanUs = nowUs + 400000LL;

  att::Ds3231Reading reading = {0, false};
  if (readRuntimeRtcSample(app, reading) != att::DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < att::EPOCH_2020) return;
  const att::Config* config = app.configBoot.config();
  if (config == nullptr) return;
  const int session = att::sessionAt(config->tt, config->params, reading.epochUtc);

  if (session < 0 || session > 1) {
    // Preserve a just-completed scan result briefly, then clearly show that
    // fingerprints are not being accepted outside the timetable window.
    if (!app.scanPromptVisible && app.scanFeedbackUntilUs != 0 &&
        nowUs < app.scanFeedbackUntilUs) return;
    if (app.scanPromptVisible || app.lastScanResult == nullptr ||
        strcmp(app.lastScanResult, "waiting_for_class") != 0) {
      app.idleCarousel.reset();
      app.scanPromptVisible = false;
      app.scanFeedbackUntilUs = 0;
      app.lastScanResult = "waiting_for_class";
    }
    showIdleFrame(app, nowUs);
    return; // never poll/search outside class windows
  }

  if (app.scanPromptVisible) {
    // The scan lease is already open. Let the sensor client finish its current
    // capture/search cycle before attempting another transaction.
    return;
  }
  if (app.scanFeedbackUntilUs != 0 && nowUs < app.scanFeedbackUntilUs)
    return;
  if (owner.beginScan(reading.epochUtc)) {
    app.idleCarousel.reset();
    if (app.oledReady)
      (void)app.oled.showAttendanceMessage("READY", "PLACE FINGER");
    app.scanPromptVisible = true;
    app.scanFeedbackUntilUs = 0;
    app.lastScanResult = "scanning";
    // No blocking UI or HTTP work is performed while the sensor's scan lease is active.
  } else if (app.lastScanResult == nullptr ||
             strcmp(app.lastScanResult, "admission_wait") != 0) {
    // Do not invite a finger when the log-backed admission projection has not
    // released the scan lease yet. The next bounded service pass retries.
    app.idleCarousel.reset();
    if (app.oledReady)
      (void)app.oled.showAttendanceMessage("PLEASE WAIT", "CHECKING CLASS");
    app.lastScanResult = "admission_wait";
  }
}

bool enterRecoveryOnlyMode(Application& app) {
  if (app.health.state() != att::H_STORAGE_FAULT ||
      !app.recoveryPresenceReady || !app.recoveryPresence.asserted() ||
      app.recoveryService == nullptr || app.adminSessions == nullptr ||
      app.adminSessions->faulted() || app.adminAuthHttpHandler == nullptr ||
      app.pinAuthRuntime.faulted() || app.adminPinWriter == nullptr ||
      !app.adminPinWriter->ready())
    return false;

  if (!app.recoveryService->begin(app.recoveryPresence)) return false;
  app.recoveryMode = true;
  app.faulted = false;
  return true;
}

bool startConfiguredAp(Application& app) {
  const att::Config* config = app.configBoot.config();
  if (config == nullptr) return false;

  char passphrase[att::AP_CREDENTIAL_BUFFER_SIZE] = {};
  const att::ApCredentialResult credential =
      app.apCredentialBootstrap.prepare(att::CFG_BOOT_READY,
                                        config->wifiPass, passphrase);
  if (credential.code != att::APCB_OK || credential.disclose) {
    secureClear(passphrase, sizeof passphrase);
    return false;
  }

  const size_t length = strlen(passphrase);
  const bool started = app.wifiAp.start(
      att::kProductionSoftApSsid,
      sizeof(att::kProductionSoftApSsid) - 1,
      passphrase, length, att::kProductionSoftApChannel);
  secureClear(passphrase, sizeof passphrase);
  return started && app.wifiAp.ready();
}

void startApplication(Application& app) {
  const att::ConfigBootResult boot = app.configBoot.start();
  app.bootState = boot.state;
  if (boot.state != att::CFG_BOOT_NEEDS_SETUP &&
      boot.state != att::CFG_BOOT_READY) {
    app.faulted = true;
    // ConfigBoot fails before normal peripheral startup. The global NVS
    // safety guard has already passed, so this terminal path may initialize
    // only I2C/OLED to show a non-secret service code. It never retries NVS,
    // repairs or erases data, or starts AP/HTTP.
    if (app.i2cBus.begin(kSdaPin, kSclPin, kI2cClockHz)) {
      const att::Esp32OledInitCode oledResult = app.oled.begin();
      app.oledReady = oledResult == att::EOI_BUFFER_READY ||
                      oledResult == att::EOI_ALREADY_INITIALIZED;
      if (app.oledReady) (void)app.oled.showConfigFault();
    }
    return;
  }

  // Configure the board-confirmed AS608 TCH line as a passive, input-only
  // local-presence signal. Failure disables only the future recovery path; it
  // must not block normal startup. No recovery mount or operation is started.
  app.recoveryPresenceReady = app.recoveryPresence.begin();

  // E35: construct the live salted PBKDF2 verifier only from the exact
  // validated READY config. NEEDS_SETUP has no admin PIN credential yet.
  if (app.bootState == att::CFG_BOOT_READY) {
    app.pinConfigRuntime = new (app.pinConfigRuntimeMemory)
        att::Esp32MbedTlsPinConfigRuntime(
            app.configBoot, att::kE35AdminPinPbkdf2Iterations);
    if (!app.pinConfigRuntime->ready() ||
        app.pinConfigRuntime->verifier() == nullptr) {
      app.faulted = true;
      return;
    }
    app.adminPinWriter = new (app.adminPinWriterMemory)
        att::Esp32MbedTlsAdminPinWriter(
            app.configBoot, *app.pinConfigRuntime, app.runtimeRandom,
            att::kE35AdminPinPbkdf2Iterations);
    if (!app.adminPinWriter->ready()) {
      app.faulted = true;
      return;
    }
  }

  const bool busReady = app.i2cBus.begin(kSdaPin, kSclPin, kI2cClockHz);
  if (busReady) {
    const att::Esp32OledInitCode oledResult = app.oled.begin();
    app.oledReady = oledResult == att::EOI_BUFFER_READY ||
                    oledResult == att::EOI_ALREADY_INITIALIZED;
  }

  // READY boot only: observe the DS3231 UTC calendar and OSF without changing
  // RTC state or establishing clock trust.
  observeRtcStartup(app);

  // Both first-run and READY must prove that the live partition table contains
  // the exact signed storage geometry before initializing lockout counters,
  // disclosing AP credentials, or accepting provisioning. This is a read-only
  // lookup; the mount path repeats the check immediately before VFS register.
  if (!app.storageInitBackend.hasSignedPartitionGeometry()) {
    if (app.oledReady) (void)app.oled.showStorageFault();
    app.faulted = true;
    return;
  }

  // Load both durable lockout domains before exposing either AP state.
  // NEEDS_SETUP may initialize an absent counter in each distinct namespace.
  // READY uses the administrator PIN attempt counter. The separate setup-code
  // attempt domain is initialized only during first-run provisioning.
  if (!app.pinAuthRuntime.start(monotonicSeconds())) {
    if (app.oledReady) (void)app.oled.showAuthFault();
    app.faulted = true;
    return;
  }

  // The setup code is a one-time first-run pairing challenge, not a routine
  // administrator credential. READY administration uses only the admin PIN.
  if (app.bootState == att::CFG_BOOT_NEEDS_SETUP)
    (void)app.setupCodeAuthRuntime.start(monotonicSeconds());

  // NEEDS_SETUP uses a distinct session-free EP_SETUP boundary. The AP may
  // remain status-only if attsetup is unavailable; route registration below
  // requires this exact durable runtime and the RAM-only code verifier.
  if (app.bootState == att::CFG_BOOT_NEEDS_SETUP) {
    app.firstRunSequence = new (app.firstRunSequenceMemory)
        att::FirstRunSetupCodeSequence(app.apSetup, app.setupCodeVerifier);
    if (app.setupCodeAuthRuntime.ready()) {
      att::PinAuthCoordinator* setupAuth =
          app.setupCodeAuthRuntime.coordinator();
      if (setupAuth == nullptr) {
        app.faulted = true;
        return;
      }
      app.firstRunSetupBoundary = new (app.firstRunSetupBoundaryMemory)
          att::FirstRunSetupCodeBoundary(
              *setupAuth, app.setupCodeVerifier, *app.firstRunSequence,
              app.firstRunClockProvisioner, app.firstRunCheckpointFinalizer,
              app.initialPinPreparer);
    }
  }

  // Bind the READY-only request/session graph to the exact long-lived owners.
  // The PIN session boundary can exist only after the durable attsec runtime
  // and the live PBKDF2 verifier are both ready. The READY auth handler exposes
  // login, logout and current-PIN change only; the first-run OLED challenge is
  // never reused as a routine administrator credential.
  if (app.bootState == att::CFG_BOOT_READY) {
    att::PinAuthCoordinator* pinAuth = app.pinAuthRuntime.coordinator();
    att::Esp32MbedTlsPinVerifier* pinVerifier =
        app.pinConfigRuntime != nullptr
            ? app.pinConfigRuntime->verifier() : nullptr;
    if (pinAuth == nullptr || pinVerifier == nullptr) {
      app.faulted = true;
      return;
    }
    app.adminSessions = new (app.adminSessionsMemory)
        att::AdminSessionBoundary(*pinAuth, *pinVerifier, app.runtimeRandom);

    if (app.adminPinWriter == nullptr || !app.adminPinWriter->ready()) {
      app.faulted = true;
      return;
    }
    app.adminAuthHttpHandler = new (app.adminAuthHttpHandlerMemory)
        att::Esp32AdminAuthHttpHandler(
            *app.adminSessions, app.health, *app.adminPinWriter,
            monotonicSeconds, app.faulted);
  }

  // Storage is started only for a validated existing configuration. A
  // partition-layout, mount or read-only log-validation failure blocks AP
  // and HTTP startup. NEEDS_SETUP never touches the attendance partition.
  if (app.bootState == att::CFG_BOOT_READY && !startStorage(app)) {
    if (app.closureCheckpointFaulted) {
      if (app.oledReady) (void)app.oled.showConfigFault();
      app.faulted = true;
      return;
    }

    updateRuntimeHealth(app);
    if (app.oledReady) (void)app.oled.showStorageFault();
    if (!enterRecoveryOnlyMode(app)) {
      app.faulted = true;
      return;
    }
  }

  // NEEDS_SETUP treats attendance storage as not applicable. READY reaches
  // this point only after MonthSet/LogStore/projection replay with append access closed.
  updateRuntimeHealth(app);
  if (app.bootState == att::CFG_BOOT_READY && !app.recoveryMode &&
      app.attendanceGate != nullptr && startClockConfirmation(app)) {
    // Clock service composition is established before opening the append gate.
    // If RTC/auth owners are unavailable, normal storage mutation stays closed.
    app.clockWritesAuthorized =
        app.attendanceGate->authorizeStorageWritesAfterReplay();
    if (app.clockWritesAuthorized) updateRuntimeHealth(app);
  }
  if (app.bootState == att::CFG_BOOT_READY)
    showHealthStatusWhenChanged(app);

  if (app.bootState == att::CFG_BOOT_NEEDS_SETUP) {
    if (!busReady || !app.oledReady) {
      app.faulted = true;
      return;
    }
    if (app.firstRunSequence == nullptr) {
      app.faulted = true;
      return;
    }
    const att::FirstRunSetupCodeSequenceCode sequenceResult =
        app.firstRunSequence->start();
    if (sequenceResult != att::FSC_WAITING_FOR_CLIENT) {
      app.faulted = true;
      return;
    }
    app.apReady = app.wifiAp.ready();
  } else {
    app.apReady = startConfiguredAp(app);
    if (!app.apReady) {
      if (app.recoveryMode && app.recoveryService != nullptr)
        (void)app.recoveryService->end();
      app.faulted = true;
      return;
    }
  }

  // Start the sensor only after AP startup has returned: Wi-Fi initialization
  // can block, so sending the first UART command any earlier could let its
  // bounded response deadline expire before the loop starts polling Serial2.
  if (app.bootState == att::CFG_BOOT_READY && !app.recoveryMode &&
      !startAs608Runtime(app)) {
    app.sensorStartupFaulted = true;
    updateRuntimeHealth(app);
  }

  if (app.apReady) registerHttpRoutes(app);
}

}  // namespace

void executePendingAdminMaintenance(Application& app) {
  const AdminMaintenanceAction action = app.maintenanceAction;
  if (action == ADMIN_MAINTENANCE_NONE) return;
  if (action == ADMIN_MAINTENANCE_RESTART) {
    esp_restart();
    return;  // Keep the executor safe even if a platform stub unexpectedly returns.
  }

  if (app.as608AttendanceOwner == nullptr ||
      app.as608AttendanceOwner->clearTemplatesStatus() == att::AS608_CLEAR_IDLE) {
    app.maintenanceAction = ADMIN_MAINTENANCE_NONE;
    app.maintenanceError = "fingerprint_clear_not_started";
    return;
  }

  // The HTTP handler has already returned its 202 response. Continue the
  // asynchronous UART transaction here, outside WebServer, while feeding the
  // task watchdog. Normal scanning/enrollment cannot run during this section.
  att::As608ClearTemplatesStatus sensorStatus =
      app.as608AttendanceOwner->clearTemplatesStatus();
  while (sensorStatus == att::AS608_CLEAR_BUSY) {
    sensorStatus = app.as608AttendanceOwner->stepClearAllTemplates();
#if defined(ESP_PLATFORM)
    (void)esp_task_wdt_reset();
#endif
    delay(1);
  }
  if (sensorStatus != att::AS608_CLEAR_DONE) {
    app.maintenanceAction = ADMIN_MAINTENANCE_NONE;
    app.maintenanceError = "fingerprint_clear_failed_data_preserved";
    app.sensorStartupFaulted = true;
    updateRuntimeHealth(app);
    showHealthStatusWhenChanged(app);
    return;  // Never format attendance data if sensor clearing is uncertain.
  }

  if (app.storageOwner == nullptr || !app.storageOwner->end() ||
      !app.storageMountBackend.formatSignedPartition()) {
    app.maintenanceAction = ADMIN_MAINTENANCE_NONE;
    app.maintenanceError = "storage_format_failed_sensor_cleared";
    // A failed unmount/format is a storage integrity fault. Reboot to force
    // normal no-format validation/recovery; never serve against stale handles.
    esp_restart();
    return;
  }

  if (action == ADMIN_MAINTENANCE_FACTORY_RESET &&
      !clearApplicationNvsForFactoryReset()) {
    app.maintenanceAction = ADMIN_MAINTENANCE_NONE;
    app.maintenanceError = "factory_settings_clear_failed";
    // The config namespace is erased last. A reboot re-evaluates the committed
    // NVS state and either enters first-run setup or fails closed on corruption.
    esp_restart();
    return;
  }

  // Records reset preserves config, timetable, Wi-Fi and admin PIN. Factory
  // reset additionally removes every application-owned NVS namespace.
  esp_restart();
}

void setup() {
  // Establish an inactive HIGH output before NVS checks or peripheral startup.
  // No boot chirp is emitted; GPIO23 is active-low on the installed module.
  (void)gBuzzer.begin();
  const att::Esp32NvsStartupStatus nvs =
      att::Esp32NvsStartupGuard::status();
  if (!nvs.safeForApplication) {
    return;
  }

  static Application application;
  gApp = &application;
  startApplication(application);

#if defined(ESP_PLATFORM)
  // Subscribe only after one-time setup/replay has completed. Starting the
  // loop watchdog earlier can turn a slow but valid large-log boot into a
  // watchdog reset loop before loop() has a chance to feed it.
  // The timeout must outlast one blocking WebServer::handleClient() call
  // (headers + raw body + close); see loop_watchdog_policy.h.
  const esp_task_wdt_config_t twdtConfig = {
      att::kLoopWatchdogTimeoutMs,   // timeout_ms
      0,      // idle_core_mask
      true    // trigger_panic
  };
  esp_err_t watchdogResult = esp_task_wdt_reconfigure(&twdtConfig);
  if (watchdogResult == ESP_ERR_INVALID_STATE) {
    watchdogResult = esp_task_wdt_init(&twdtConfig);
    // Another initializer may have won the race; apply our requested policy.
    if (watchdogResult == ESP_ERR_INVALID_STATE)
      watchdogResult = esp_task_wdt_reconfigure(&twdtConfig);
  }
  if (watchdogResult == ESP_OK) {
    const esp_err_t subscribeResult = esp_task_wdt_add(NULL);
    // INVALID_STATE here means this task is already subscribed; the watchdog
    // is still supervising it. Other failures must not be silently ignored.
    if (subscribeResult != ESP_OK &&
        subscribeResult != ESP_ERR_INVALID_STATE)
      watchdogResult = subscribeResult;
  }
  if (watchdogResult != ESP_OK) {
    if (gApp != nullptr) gApp->faulted = true;
  }
#endif
}

void loop() {
  const uint8_t buzzerMode = gApp != nullptr
      ? currentBuzzerSoundMode(*gApp)
      : static_cast<uint8_t>(att::BUZZER_SOUND_SILENT);
  gBuzzer.service(millis(), buzzerMode);
#if defined(ESP_PLATFORM)
  (void)esp_task_wdt_reset();
#endif
  if (gApp != nullptr) {
    serviceRecoveryLease(*gApp);
    // Recovery mode is a separate application composition. It serves only
    // root/status, minimal ADMIN auth, and four recovery routes; stale normal
    // projections, AS608 startup, setup code and ordinary domain routes never run.
    if (gApp->recoveryMode) {
      if (gApp->recoveryService == nullptr ||
          gApp->recoveryService->cleanupPending()) return;
      // Keep only the fixed status/root surface alive after a completed or
      // revoked recovery lease. The registered recovery handler remains
      // harmless because the service itself now returns READ_ONLY when ended.
      serviceAdminSessions(*gApp);
      if (gApp->httpReady && gApp->apReady && !gApp->faulted &&
          !gBuzzer.busy())
        gApp->http.handleClient();
      if (gApp->maintenanceAction != ADMIN_MAINTENANCE_NONE) {
        executePendingAdminMaintenance(*gApp);
        if (gApp->maintenanceAction != ADMIN_MAINTENANCE_NONE) return;
      }
      serviceRecoveryLease(*gApp);
      return;
    }
    // Normal projections and peripherals belong to the pre-recovery storage
    // owner. Once it has been relinquished, only cleanup/retry may run until
    // an operator performs a fresh physical reboot.
    if (gApp->recoveryService != nullptr &&
        gApp->recoveryService->restartRequired()) return;
    // Reuse one RTC/I2C sample for the complete runtime service pass. Enable it
    // before serviceAs608Runtime(): a startup transition can update clock health,
    // while the periodic health service also runs the closure-checkpoint lifecycle.
    // Disable it before HTTP handlers, which may mutate clock/configuration state.
    // Reap expired bearer-session records on the same serialized owner.
    serviceAdminSessions(*gApp);
    gApp->loopRtcCacheEnabled = true;
    gApp->loopRtcReadAttempted = false;
    serviceAs608Runtime(*gApp);
    serviceRuntimeClockHealth(*gApp);
    serviceClosureScheduler(*gApp);
    serviceAutomaticAttendance(*gApp);
    // Long transient OLED lines scroll cooperatively on this serialized UI
    // owner. The driver performs at most one bounded I2C refresh per due step.
    if (gApp->oledReady)
      gApp->oled.serviceMarquee(esp_timer_get_time());
    gApp->loopRtcCacheEnabled = false;

    // E33: service RAM-only setup-code expiry on the same serialized owner
    // before a request can verify it and again after synchronous HTTP returns.
    // First-run issue/reissue is advanced below only after the DHCP latch.
    if (!gApp->setupCodeVerifier.serviceExpiry() &&
        gApp->bootState == att::CFG_BOOT_NEEDS_SETUP)
      gApp->faulted = true;
    // First-run code disclosure is enabled only after the setup-only raw POST
    // handler and its three framing headers are registered during startup.
    if (!gApp->faulted &&
        gApp->firstRunSetupRouteReady &&
        gApp->bootState == att::CFG_BOOT_NEEDS_SETUP &&
        gApp->firstRunSequence != nullptr &&
        gApp->firstRunSetupBoundary != nullptr &&
        !gApp->firstRunSetupBoundary->faulted() &&
        !gApp->firstRunSetupBoundary->completed() &&
        !gApp->setupCodeVerifier.faulted()) {
      (void)gApp->firstRunSequence->issueAfterClientAssigned();
    }
    // The WebServer is synchronous. Defer requests while sensor startup is
    // polling so a stalled client cannot starve UART RX past its deadline.
    const bool as608StartupInFlight =
        gApp->sensorStartupStarted && !gApp->sensorStartupReady &&
        !gApp->sensorStartupFaulted;
    const bool as608TransactionInFlight =
        gApp->as608AttendanceOwner != nullptr &&
        (gApp->as608AttendanceOwner->scanActive() ||
         (gApp->as608AttendanceOwner->enrollmentActive() &&
          gApp->as608AttendanceOwner->enrollmentOperationPending()));
    const bool firstRunRestartPending =
        gApp->bootState == att::CFG_BOOT_NEEDS_SETUP &&
        gApp->firstRunSetupBoundary != nullptr &&
        gApp->firstRunSetupBoundary->completed();
    // Once the one-way config commit has completed, keep the setup composition
    // reachable for GET / and GET /api/status while OLED cleanup/restart is
    // pending. This composition has no READY/admin routes, and the setup
    // boundary rejects every second provisioning attempt after completion.
    const bool httpMayServe = !gApp->faulted || firstRunRestartPending;
    if (gApp->httpReady && gApp->apReady && httpMayServe &&
        !as608StartupInFlight && !as608TransactionInFlight &&
        !gBuzzer.busy()) {
      gApp->http.handleClient();
      // READY maintenance actions are deliberately deferred until the HTTP
      // handler has returned its 202 response. Execute here on the same
      // serialized owner task, before any scan/closure service can run again.
      if (gApp->maintenanceAction != ADMIN_MAINTENANCE_NONE) {
        executePendingAdminMaintenance(*gApp);
        if (gApp->maintenanceAction != ADMIN_MAINTENANCE_NONE) return;
      }
      serviceRecoveryLease(*gApp);
      if (gApp->recoveryService != nullptr &&
          gApp->recoveryService->restartRequired()) return;
      if (!gApp->setupCodeVerifier.serviceExpiry() &&
          gApp->bootState == att::CFG_BOOT_NEEDS_SETUP)
        gApp->faulted = true;
    }
    // A committed config is not enough: never reboot while the OLED may
    // still show the AP credential. Retry only the blank transfer, throttled.
    if (gApp->firstRunSetupBoundary != nullptr &&
        gApp->firstRunSetupBoundary->completed() &&
        !gApp->firstRunSetupBoundary->restartReady() &&
        gApp->firstRunSequence != nullptr) {
      const int64_t nowUs = esp_timer_get_time();
      if (nowUs >= gApp->nextProvisionClearRetryUs) {
        gApp->nextProvisionClearRetryUs = nowUs + 1000000LL;
        (void)gApp->firstRunSequence->retryBlankFrame();
      }
    }
    // Run only after handleClient() returns: the HTTP response has left its
    // handler and the OLED blank transfer has completed.
    if (gApp->firstRunSetupBoundary != nullptr &&
        gApp->firstRunRestart.service(*gApp->firstRunSetupBoundary,
                                      gApp->applicationRestart)) {
      gApp->faulted = true;
    }
  }
  delay(2);
}
