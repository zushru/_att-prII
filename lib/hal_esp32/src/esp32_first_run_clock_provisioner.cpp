#include "esp32_first_run_clock_provisioner.h"
#include "time_rules.h"
#include "api_logic.h"
namespace att {
bool Esp32FirstRunClockProvisioner::setAndConfirm(
    uint32_t requestedUtcEpoch, int16_t utcOffsetMin,
    uint32_t& confirmedUtcEpoch) {
  if (faulted_ || requestedUtcEpoch < EPOCH_2020 ||
      utcOffsetMin < -720 || utcOffsetMin > 840 ||
      !apiDaySupported(dayNumber(requestedUtcEpoch, utcOffsetMin))) {
    faulted_ = true;
    return false;
  }
  uint64_t writeStartedUs = 0;
  if (!monotonic_.nowMicros(writeStartedUs)) {
    faulted_ = true;
    return false;
  }
  if (rtc_.writeUtcEpoch(requestedUtcEpoch, 0) != DS3231_OK) {
    faulted_ = true;
    return false;
  }
  Ds3231Reading afterWrite = {0, false};
  uint64_t afterWriteUs = 0;
  if (rtc_.readUtcEpoch(0, afterWrite) != DS3231_OK ||
      !monotonic_.nowMicros(afterWriteUs) ||
      !rtcSampleWithinWriteWindow(requestedUtcEpoch, afterWrite.epochUtc,
                                  writeStartedUs, afterWriteUs) ||
      !apiDaySupported(dayNumber(afterWrite.epochUtc, utcOffsetMin))) {
    faulted_ = true;
    return false;
  }
  if (rtc_.clearOscillatorStopFlag() != DS3231_OK) {
    faulted_ = true;
    return false;
  }
  Ds3231Reading finalReading = {0, false};
  uint64_t finalReadUs = 0;
  if (rtc_.readUtcEpoch(0, finalReading) != DS3231_OK ||
      finalReading.oscillatorStopped ||
      !monotonic_.nowMicros(finalReadUs) || finalReadUs < afterWriteUs ||
      !rtcSampleWithinWriteWindow(requestedUtcEpoch, finalReading.epochUtc,
                                  writeStartedUs, finalReadUs) ||
      !apiDaySupported(dayNumber(finalReading.epochUtc, utcOffsetMin))) {
    faulted_ = true;
    return false;
  }
  confirmedUtcEpoch = finalReading.epochUtc;
  return true;
}
}
