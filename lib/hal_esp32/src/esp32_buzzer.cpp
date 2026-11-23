#include "esp32_buzzer.h"

#include <Arduino.h>

namespace att {
namespace {

const uint16_t kShortPulseMs = 70;
const uint16_t kVeryShortPulseMs = 50;
const uint16_t kLongPulseMs = 230;
const uint16_t kShortGapMs = 100;
const uint16_t kLongGapMs = 130;

bool isRoutineAcknowledgement(BuzzerPattern pattern) {
  return pattern == BUZZER_PATTERN_PRESENT ||
      pattern == BUZZER_PATTERN_LATE ||
      pattern == BUZZER_PATTERN_ENROLLMENT_STEP;
}

bool isCriticalAlert(BuzzerPattern pattern) {
  return pattern == BUZZER_PATTERN_NOT_SAVED ||
      pattern == BUZZER_PATTERN_FAULT;
}

}  // namespace

Esp32Buzzer::Esp32Buzzer()
    : initialized_(false), busy_(false), phaseOn_(false),
      outputActive_(false), pulsesRemaining_(0), onMs_(0), gapMs_(0),
      phaseStartedMs_(0) {}

bool Esp32Buzzer::begin() {
  if (initialized_) {
    stop();
    return true;
  }

  // Set the output latch high before enabling output mode. On ESP32 Arduino,
  // writing HIGH while still an input also enables the internal pull-up during
  // the brief transition. An external pull-up is still recommended for reset.
  digitalWrite(kPin, HIGH);
  pinMode(kPin, OUTPUT);
  digitalWrite(kPin, HIGH);

  initialized_ = true;
  busy_ = false;
  phaseOn_ = false;
  outputActive_ = false;
  pulsesRemaining_ = 0;
  onMs_ = gapMs_ = 0;
  phaseStartedMs_ = 0;
  return true;
}

void Esp32Buzzer::drive_(bool active) {
  if (!initialized_ || outputActive_ == active) return;
  digitalWrite(kPin, active ? LOW : HIGH);
  outputActive_ = active;
}

void Esp32Buzzer::stop() {
  if (!initialized_) return;
  busy_ = false;
  phaseOn_ = false;
  pulsesRemaining_ = 0;
  drive_(false);
}

bool Esp32Buzzer::patternSpec_(BuzzerPattern pattern, uint8_t soundMode,
                               PatternSpec& spec) {
  spec.pulses = 0;
  spec.onMs = 0;
  spec.gapMs = 0;

  if (soundMode >= BUZZER_SOUND_SILENT)
    return false;

  if (soundMode == BUZZER_SOUND_QUIET) {
    // Quiet mode suppresses routine positive acknowledgements. Warnings collapse
    // to one brief pulse; unsaved/uncertain/fault conditions retain a distinct
    // two-pulse alert. An active digital buzzer has no independent volume control.
    if (isRoutineAcknowledgement(pattern)) return false;
    if (isCriticalAlert(pattern)) {
      spec.pulses = 2;
      spec.onMs = 120;
      spec.gapMs = 120;
      return true;
    }
    switch (pattern) {
      case BUZZER_PATTERN_ALREADY_RECORDED:
      case BUZZER_PATTERN_BAD_IMAGE:
      case BUZZER_PATTERN_NO_MATCH:
      case BUZZER_PATTERN_OUTSIDE_WINDOW:
      case BUZZER_PATTERN_NO_CLASS:
      case BUZZER_PATTERN_NOT_ACTIVE:
        spec.pulses = 1;
        spec.onMs = 60;
        return true;
      default:
        return false;
    }
  }

  switch (pattern) {
    case BUZZER_PATTERN_PRESENT:
    case BUZZER_PATTERN_ENROLLMENT_STEP:
      spec.pulses = 1;
      spec.onMs = kShortPulseMs;
      return true;
    case BUZZER_PATTERN_LATE:
      spec.pulses = 2;
      spec.onMs = kShortPulseMs;
      spec.gapMs = kShortGapMs;
      return true;
    case BUZZER_PATTERN_ALREADY_RECORDED:
    case BUZZER_PATTERN_OUTSIDE_WINDOW:
    case BUZZER_PATTERN_NO_CLASS:
      spec.pulses = 1;
      spec.onMs = kLongPulseMs;
      return true;
    case BUZZER_PATTERN_BAD_IMAGE:
      spec.pulses = 1;
      spec.onMs = kVeryShortPulseMs;
      return true;
    case BUZZER_PATTERN_NO_MATCH:
    case BUZZER_PATTERN_NOT_ACTIVE:
      spec.pulses = 2;
      spec.onMs = kLongPulseMs;
      spec.gapMs = kLongGapMs;
      return true;
    case BUZZER_PATTERN_NOT_SAVED:
    case BUZZER_PATTERN_FAULT:
      spec.pulses = 3;
      spec.onMs = kLongPulseMs;
      spec.gapMs = kLongGapMs;
      return true;
    default:
      return false;
  }
}

void Esp32Buzzer::play(BuzzerPattern pattern, uint8_t soundMode,
                       uint32_t nowMs) {
  if (!initialized_) return;

  PatternSpec spec = {0, 0, 0};
  if (!patternSpec_(pattern, soundMode, spec)) return;

  // A new result supersedes an old pattern. This is safe because the caller only
  // emits on completed scan results and enrollment prompt transitions, not polls.
  pulsesRemaining_ = spec.pulses;
  onMs_ = spec.onMs;
  gapMs_ = spec.gapMs;
  phaseStartedMs_ = nowMs;
  phaseOn_ = true;
  busy_ = true;
  drive_(true);  // Active-low: LOW is ON.
}

void Esp32Buzzer::service(uint32_t nowMs, uint8_t soundMode) {
  if (!initialized_) return;
  if (soundMode >= BUZZER_SOUND_SILENT) {
    stop();
    return;
  }
  if (!busy_) return;

  // Unsigned subtraction is intentionally used for millis() wrap safety.
  const uint32_t elapsed = nowMs - phaseStartedMs_;
  if (phaseOn_) {
    if (elapsed < onMs_) return;
    drive_(false);
    phaseOn_ = false;
    if (pulsesRemaining_ > 0) --pulsesRemaining_;
    if (pulsesRemaining_ == 0) {
      busy_ = false;
      return;
    }
    phaseStartedMs_ = nowMs;
    return;
  }

  if (elapsed < gapMs_) return;
  phaseStartedMs_ = nowMs;
  phaseOn_ = true;
  drive_(true);
}

}  // namespace att
