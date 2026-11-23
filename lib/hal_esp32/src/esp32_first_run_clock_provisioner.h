#pragma once
#include "setup_code_sequence.h"
#include "clock_confirmation.h"
#include "ds3231_clock.h"
namespace att {
class Esp32FirstRunClockProvisioner final : public IFirstRunClockProvisioner {
 public:
  Esp32FirstRunClockProvisioner(Ds3231Clock& rtc,
                                IClockMonotonicSource& monotonic)
      : rtc_(rtc), monotonic_(monotonic), faulted_(false) {}
  bool setAndConfirm(uint32_t requestedUtcEpoch, int16_t utcOffsetMin,
                     uint32_t& confirmedUtcEpoch) override;
  bool faulted() const { return faulted_; }
 private:
  Ds3231Clock& rtc_;
  IClockMonotonicSource& monotonic_;
  bool faulted_;
};
}
