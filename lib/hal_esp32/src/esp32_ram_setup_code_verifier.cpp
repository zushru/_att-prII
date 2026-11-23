#include "esp32_ram_setup_code_verifier.h"

#include <string.h>

namespace att {
namespace {

void secureWipe(void* value, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(value);
  while (length--) *p++ = 0;
}

bool generateSetupCode(Esp32SecureRandom& random, char output[7]) {
  if (output == nullptr) return false;
  uint8_t entropy[16] = {};
  char candidate[7] = {};
  bool generated = false;
  for (uint8_t attempt = 0; attempt < 4 && !generated; ++attempt) {
    if (!random.fill(entropy, sizeof entropy)) break;
    size_t digits = 0;
    for (size_t i = 0; i < sizeof entropy && digits < 6; ++i) {
      // 0..249 contains 25 equally likely values for each decimal digit.
      // Reject 250..255 instead of introducing modulo bias.
      if (entropy[i] >= 250u) continue;
      candidate[digits++] = static_cast<char>('0' + entropy[i] % 10u);
    }
    generated = digits == 6;
  }
  if (generated) {
    candidate[6] = '\0';
    memcpy(output, candidate, sizeof candidate);
  }
  secureWipe(entropy, sizeof entropy);
  secureWipe(candidate, sizeof candidate);
  return generated;
}

}  // namespace

Esp32RamSetupCodeVerifier::Esp32RamSetupCodeVerifier(
    Esp32SecureRandom& random, IClockMonotonicSource& monotonic,
    Esp32Oled& oled)
    : random_(random), monotonic_(monotonic), oled_(oled), code_{0},
      issuedAtMicros_(0), lastObservedMicros_(0),
      lastDisplayedMinutesRemaining_(0), reissueAfterMicros_(0),
      hasObservedMicros_(false), active_(false), faulted_(false),
      reissuePending_(false) {}

Esp32RamSetupCodeVerifier::~Esp32RamSetupCodeVerifier() {
  if (active_) (void)requestBlank_();
  wipeCode_();
  active_ = false;
  issuedAtMicros_ = 0;
}

void Esp32RamSetupCodeVerifier::wipeCode_() {
  secureWipe(code_, sizeof code_);
}

bool Esp32RamSetupCodeVerifier::requestBlank_() {
  return oled_.clear() == EOA_TRANSFER_REQUESTED;
}

bool Esp32RamSetupCodeVerifier::observeMonotonic_(uint64_t nowMicros) {
  if (hasObservedMicros_ && nowMicros < lastObservedMicros_) return false;
  lastObservedMicros_ = nowMicros;
  hasObservedMicros_ = true;
  return true;
}

void Esp32RamSetupCodeVerifier::latchFault_() {
  active_ = false;
  issuedAtMicros_ = 0;
  lastDisplayedMinutesRemaining_ = 0;
  reissueAfterMicros_ = 0;
  reissuePending_ = false;
  wipeCode_();
  faulted_ = true;
}

void Esp32RamSetupCodeVerifier::failClosed_() {
  latchFault_();
  (void)requestBlank_();
}

bool Esp32RamSetupCodeVerifier::expireAndNotify_() {
  // Invalidate the code in RAM before touching the display. The panel now
  // receives only a non-secret expiry notice, never the expired digits.
  active_ = false;
  issuedAtMicros_ = 0;
  lastDisplayedMinutesRemaining_ = 0;
  wipeCode_();
  if (oled_.showCodeExpired() != EOA_TRANSFER_REQUESTED) {
    // One bounded fallback clear is allowed if the notice itself cannot be
    // transferred. Failure still latches the verifier; never retry in a loop.
    (void)requestBlank_();
    latchFault_();
    return false;
  }
  const uint64_t maxValue = static_cast<uint64_t>(-1);
  reissueAfterMicros_ = lastObservedMicros_ > maxValue - 2000000ULL
      ? maxValue : lastObservedMicros_ + 2000000ULL;
  reissuePending_ = true;
  return true;
}

bool Esp32RamSetupCodeVerifier::issue() {
  if (faulted_ || reissuePending_) return false;
  if (!random_.rfStarted()) {
    failClosed_();
    return false;
  }

  uint64_t observedMicros = 0;
  if (!monotonic_.nowMicros(observedMicros) ||
      !observeMonotonic_(observedMicros)) {
    failClosed_();
    return false;
  }

  if (active_) {
    if (observedMicros - issuedAtMicros_ < kLifetimeMicros) {
      return false;
    }
    // The old code is invalidated and an expiry notice is shown before any
    // replacement is issued. Hold that notice for the full bounded interval.
    if (!expireAndNotify_()) return false;
    return false;
  }

  if (!generateSetupCode(random_, code_)) {
    failClosed_();
    return false;
  }

  if (oled_.showSetupCode(code_, 6, 600) != EOA_TRANSFER_REQUESTED) {
    failClosed_();
    return false;
  }

  // Start the lifetime after the display transfer was requested. A failed or
  // regressing clock read invalidates the newly displayed code immediately.
  uint64_t issuedAt = 0;
  if (!monotonic_.nowMicros(issuedAt) || issuedAt < observedMicros ||
      !observeMonotonic_(issuedAt)) {
    failClosed_();
    return false;
  }

  issuedAtMicros_ = issuedAt;
  lastDisplayedMinutesRemaining_ = 10;
  active_ = true;
  return true;
}

PinVerifyOutcome Esp32RamSetupCodeVerifier::verify(const char* candidate,
                                                    size_t length) {
  if (faulted_) return PV_ERROR;
  if (!active_) return PV_MISMATCH;
  if (!random_.rfStarted()) {
    failClosed_();
    return PV_ERROR;
  }

  uint64_t nowMicros = 0;
  if (!monotonic_.nowMicros(nowMicros) ||
      !observeMonotonic_(nowMicros) || nowMicros < issuedAtMicros_) {
    failClosed_();
    return PV_ERROR;
  }

  if (nowMicros - issuedAtMicros_ >= kLifetimeMicros) {
    return expireAndNotify_() ? PV_EXPIRED : PV_ERROR;
  }

  // Always compare six positions. Length/null/digit validity contribute to
  // the accumulated difference; a mismatch never consumes the active code.
  uint8_t difference = static_cast<uint8_t>(
      candidate == nullptr || length != 6 ? 1 : 0);
  for (size_t i = 0; i < 6; ++i) {
    uint8_t supplied = 0;
    if (candidate != nullptr && i < length) {
      supplied = static_cast<uint8_t>(candidate[i]);
    }
    const uint8_t isDigit =
        static_cast<uint8_t>(supplied >= '0' && supplied <= '9');
    difference |= static_cast<uint8_t>(supplied ^
                                       static_cast<uint8_t>(code_[i]));
    difference |= static_cast<uint8_t>(isDigit ^ 1U);
  }

  if (difference != 0) return PV_MISMATCH;

  // Consume before attempting the blank transfer. A display/bus failure must
  // never make an already-matched code reusable or permit the protected action.
  active_ = false;
  issuedAtMicros_ = 0;
  wipeCode_();
  if (!requestBlank_()) {
    // The failed transfer was the one blank attempt; latch without retrying it.
    latchFault_();
    return PV_ERROR;
  }
  return PV_MATCH;
}

bool Esp32RamSetupCodeVerifier::serviceExpiry() {
  if (faulted_) return false;
  if (!active_ && !reissuePending_) return true;

  if (!random_.rfStarted()) {
    failClosed_();
    return false;
  }

  uint64_t nowMicros = 0;
  if (!monotonic_.nowMicros(nowMicros) ||
      !observeMonotonic_(nowMicros) ||
      (active_ && nowMicros < issuedAtMicros_)) {
    failClosed_();
    return false;
  }

  if (!active_) {
    if (nowMicros < reissueAfterMicros_) return true;
    reissuePending_ = false;
    reissueAfterMicros_ = 0;
    return true;
  }

  const uint16_t remaining = remainingSeconds(nowMicros);
  if (remaining == 0) return expireAndNotify_();

  const uint16_t minutes =
      static_cast<uint16_t>((static_cast<uint32_t>(remaining) + 59u) / 60u);
  if (minutes != lastDisplayedMinutesRemaining_) {
    // One best-effort refresh per minute. A busy shared bus must not turn the
    // 2 ms owner loop into a stream of repeated full-frame I2C attempts.
    lastDisplayedMinutesRemaining_ = minutes;
    if (oled_.showSetupCode(code_, 6, remaining) != EOA_TRANSFER_REQUESTED) {
      failClosed_();
      return false;
    }
  }
  return true;
}

uint16_t Esp32RamSetupCodeVerifier::remainingSeconds(
    uint64_t nowMicros) const {
  if (!active_ || faulted_ || nowMicros < issuedAtMicros_) return 0;
  const uint64_t elapsed = nowMicros - issuedAtMicros_;
  if (elapsed >= kLifetimeMicros) return 0;
  const uint64_t remaining = kLifetimeMicros - elapsed;
  return static_cast<uint16_t>((remaining + 999999ULL) / 1000000ULL);
}

}  // namespace att
