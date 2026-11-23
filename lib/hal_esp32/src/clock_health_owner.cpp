#include "clock_health_owner.h"
#include "api_logic.h"

namespace att {
namespace {

AdminClockDispatchResult notDispatchedResult() {
  const ApiResult authorization = {A_LOCKED, R_STATE, 0};
  const ClockConfirmResult confirmation = {
      CCF_BAD_INPUT, CA_NONE, CLOCK_APPLY_REJECTED, 0, 0, 0, false};
  const AdminClockDispatchResult value = {
      ACDC_AUTHORIZATION_DENIED, authorization, confirmation};
  return value;
}

bool clockFactsInvalid(const HealthInputs& inputs) {
  return !inputs.rtcPresent || inputs.rtcLostPower ||
      inputs.epochBelowFloor || inputs.epochBelowLog;
}

}  // namespace

ClockHealthOwner::ClockHealthOwner(
    AdminClockConfirmation& dispatcher, HealthFsm& health,
    const ConfigBootCoordinator& configBoot)
    : dispatcher_(dispatcher), health_(health), configBoot_(configBoot),
      ownersMatch_(dispatcher_.usesOwners(health_, configBoot_)),
      clockTrusted_(false) {}

HState ClockHealthOwner::update(HealthInputs inputs) {
  if (!ownersMatch_) return health_.state();
  if (clockFactsInvalid(inputs)) clockTrusted_ = false;
  inputs.clockTrusted = clockTrusted_;
  // This owner can reverify only the clock domain after its own CCF_OK path.
  // Never let a caller clear storage/config latches through a broad signal.
  inputs.reverified = false;
  return updateHealthFromConfigBoot(health_, configBoot_, inputs);
}

bool ClockHealthOwner::restoreFromCompletedJournal(
    HealthInputs currentHealth, bool journalHealthy, bool journalPending,
    bool completedRequest, uint32_t completedEpoch, uint32_t rtcEpoch,
    uint32_t logBaselineEpoch) {
  if (!ownersMatch_ || clockTrusted_ || dispatcher_.clockBlocked() ||
      !journalHealthy || journalPending || !completedRequest ||
      !currentHealth.rtcPresent || currentHealth.rtcLostPower ||
      currentHealth.epochBelowFloor || currentHealth.epochBelowLog ||
      !currentHealth.storageOk || !currentHealth.cfgOk ||
      rtcEpoch < EPOCH_2020 || completedEpoch < EPOCH_2020 ||
      rtcEpoch < completedEpoch ||
      (logBaselineEpoch != 0 && rtcEpoch < logBaselineEpoch))
    return false;

  // The journal marker is durable evidence of operator acceptance. The live
  // DS3231 OSF bit and the replayed log baseline independently prove that the
  // oscillator has not reported a stop and that time has not rolled behind data.
  // Only the clock latch is released; sensor/storage/config faults remain.
  currentHealth.clockTrusted = true;
  currentHealth.reverified = false;
  clockTrusted_ = true;
  (void)updateHealthFromConfigBoot(health_, configBoot_, currentHealth, true);
  if ((health_.mask() & HB_CLOCK) != 0) {
    // Keep the owner's volatile trust bit aligned with the health FSM if a
    // future FSM policy adds another prerequisite to clearing HB_CLOCK.
    clockTrusted_ = false;
    return false;
  }
  return true;
}

ClockHealthDispatchResult ClockHealthOwner::confirm(
    const uint8_t requestToken[16], uint32_t nowS, LogStore& log,
    MonthSet& months, const ClockConfirmRequest& request,
    HealthInputs currentHealth) {
  if (!ownersMatch_) {
    const ClockHealthDispatchResult mismatch = {
        false, false, notDispatchedResult(), health_.state()};
    return mismatch;
  }

  // Refresh the supplied current facts before authorization/decision. A clock
  // request is never allowed to piggyback the broad all-domain reverify flag.
  currentHealth.reverified = false;
  (void)update(currentHealth);

  const AdminClockDispatchResult dispatch = dispatcher_.confirm(
      requestToken, nowS, log, months, request);

  const bool verified = dispatch.code == ACDC_CONFIRMATION_INVOKED &&
      dispatch.authorization.code == A_OK &&
      dispatch.confirmation.code == CCF_OK &&
      dispatch.confirmation.clockTrusted;

  if (verified) {
    clockTrusted_ = true;
    // CCF_OK proves RTC presence, OSF-clear, epoch floor and log-baseline
    // ordering. Preserve caller-owned sensor/storage/config facts.
    currentHealth.rtcPresent = true;
    currentHealth.rtcLostPower = false;
    currentHealth.epochBelowFloor = false;
    currentHealth.epochBelowLog = false;
    currentHealth.clockTrusted = true;
    currentHealth.reverified = false;
    const HState state = updateHealthFromConfigBoot(
        health_, configBoot_, currentHealth, true);
    const ClockHealthDispatchResult accepted = {
        true, true, dispatch, state};
    return accepted;
  }

  // Once the service latches, its audit intent/RTC outcome may be ambiguous.
  // Do not carry forward a previously trusted clock across that boundary.
  // Ordinary preflight/confirmation-pending responses do not mutate trust.
  if (dispatch.code == ACDC_CONFIRMATION_INVOKED &&
      dispatcher_.clockBlocked()) {
    clockTrusted_ = false;
    currentHealth.clockTrusted = false;
    currentHealth.reverified = false;
    const HState state = updateHealthFromConfigBoot(
        health_, configBoot_, currentHealth);
    const ClockHealthDispatchResult blocked = {
        true, false, dispatch, state};
    return blocked;
  }

  const ClockHealthDispatchResult unchanged = {
      true, false, dispatch, health_.state()};
  return unchanged;
}

}  // namespace att
