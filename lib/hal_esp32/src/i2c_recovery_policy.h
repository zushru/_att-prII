#pragma once

#include <stdint.h>

namespace att {

// Small deterministic policy for recovering the shared OLED/RTC bus after
// transport failures. It intentionally knows nothing about calendar validity:
// callers must pass true only for an actual RTC transport/read failure.
class I2cRecoveryPolicy final {
 public:
  I2cRecoveryPolicy()
      : consecutiveFailures_(0), nextAttemptUs_(0) {}

  // Returns true exactly when the caller should attempt one controller
  // restart. A healthy read resets the failure streak. Invalid monotonic time
  // never triggers recovery or mutates the policy state.
  bool observeRtcTransportResult(bool failed, int64_t nowUs) {
    if (nowUs < 0) return false;
    if (!failed) {
      consecutiveFailures_ = 0;
      return false;
    }

    if (consecutiveFailures_ < kFailureThreshold) ++consecutiveFailures_;
    if (consecutiveFailures_ < kFailureThreshold || nowUs < nextAttemptUs_)
      return false;

    consecutiveFailures_ = 0;
    nextAttemptUs_ = nowUs > INT64_MAX - kCooldownUs
        ? INT64_MAX : nowUs + kCooldownUs;
    return true;
  }

  uint8_t consecutiveFailures() const { return consecutiveFailures_; }
  int64_t nextAttemptUs() const { return nextAttemptUs_; }

  static const uint8_t kFailureThreshold = 2;
  static const int64_t kCooldownUs = 30000000LL;

 private:
  uint8_t consecutiveFailures_;
  int64_t nextAttemptUs_;
};

}  // namespace att
