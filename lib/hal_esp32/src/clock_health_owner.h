#pragma once

#include "admin_clock_confirmation.h"
#include "config_runtime_guard.h"

namespace att {

struct ClockHealthDispatchResult {
  bool dispatched;
  bool clockAccepted;
  AdminClockDispatchResult admin;
  HState healthState;
};

// Serialized application-side owner for the authenticated clock operation and
// its separate HealthFsm transition. It binds to the exact HealthFsm and
// ConfigBootCoordinator already held by the dispatcher's clock service.
// confirm() and update() must share the one application task with every
// session, config, RTC, log and storage writer.
class ClockHealthOwner {
 public:
  ClockHealthOwner(AdminClockConfirmation& dispatcher, HealthFsm& health,
                   const ConfigBootCoordinator& configBoot);

  ClockHealthOwner(const ClockHealthOwner&) = delete;
  ClockHealthOwner& operator=(const ClockHealthOwner&) = delete;

  bool ready() const { return ownersMatch_; }
  bool clockTrusted() const { return clockTrusted_; }
  bool serviceBlocked() const { return dispatcher_.clockBlocked(); }

  // Refresh independent health facts. A newly observed RTC/epoch fault
  // revokes the volatile trust accepted by this owner.
  HState update(HealthInputs inputs);

  // Restore trust after an MCU restart (or a transient RTC bus disconnect) only
  // when the replayed event audit proves a completed clock transaction, no
  // transaction is pending, and the live DS3231 sample is still beyond both
  // the accepted epoch and the replayed log baseline with OSF clear. This is
  // deliberately separate from a general-purpose health reverify.
  bool restoreFromCompletedJournal(
      HealthInputs currentHealth, bool journalHealthy, bool journalPending,
      bool completedRequest, uint32_t completedEpoch, uint32_t rtcEpoch,
      uint32_t logBaselineEpoch);

  // Refreshes current health facts before request-bound authorization, then
  // performs clock confirmation itself. It never forwards broad reverified.
  // Only an invoked, authorized CCF_OK + clockTrusted result releases HB_CLOCK.
  // A service-latched ambiguous failure revokes prior trust.
  ClockHealthDispatchResult confirm(
      const uint8_t requestToken[16], uint32_t nowS, LogStore& log,
      MonthSet& months, const ClockConfirmRequest& request,
      HealthInputs currentHealth);

 private:
  AdminClockConfirmation& dispatcher_;
  HealthFsm& health_;
  const ConfigBootCoordinator& configBoot_;
  bool ownersMatch_;
  bool clockTrusted_;
};

}  // namespace att
