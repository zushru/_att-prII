#pragma once

#include <stdint.h>

namespace att {

enum BuzzerSoundMode : uint8_t {
  BUZZER_SOUND_FULL = 0,
  BUZZER_SOUND_QUIET = 1,
  BUZZER_SOUND_SILENT = 2
};

enum BuzzerPattern : uint8_t {
  BUZZER_PATTERN_PRESENT = 0,
  BUZZER_PATTERN_LATE,
  BUZZER_PATTERN_ALREADY_RECORDED,
  BUZZER_PATTERN_BAD_IMAGE,
  BUZZER_PATTERN_NO_MATCH,
  BUZZER_PATTERN_OUTSIDE_WINDOW,
  BUZZER_PATTERN_NO_CLASS,
  BUZZER_PATTERN_NOT_ACTIVE,
  BUZZER_PATTERN_NOT_SAVED,
  BUZZER_PATTERN_FAULT,
  BUZZER_PATTERN_ENROLLMENT_STEP
};

// GPIO23 drives the active-low logic input of a three-pin active-buzzer module.
// The module generates its own audio tone; this adapter only switches it on/off.
// All timing is cooperative/non-blocking and must be serviced from the main loop.
class Esp32Buzzer {
 public:
  static const uint8_t kPin = 23;

  Esp32Buzzer();
  Esp32Buzzer(const Esp32Buzzer&) = delete;
  Esp32Buzzer& operator=(const Esp32Buzzer&) = delete;

  // Preloads the inactive HIGH level before switching GPIO23 to OUTPUT. No boot
  // chirp is intentionally generated. Call at the very start of setup().
  bool begin();

  // Starts/replaces the current pattern. A new event takes precedence over an
  // unfinished pattern; callers must emit only on state/result transitions.
  void play(BuzzerPattern pattern, uint8_t soundMode, uint32_t nowMs);

  // Advances at most one pattern edge per call. Pass the current persisted
  // soundMode so a transition to Silent immediately cancels an active pattern.
  void service(uint32_t nowMs, uint8_t soundMode);

  // Cancels playback and forces the physical output inactive (HIGH).
  void stop();

  bool initialized() const { return initialized_; }
  bool busy() const { return busy_; }
  bool outputActive() const { return outputActive_; }

 private:
  struct PatternSpec {
    uint8_t pulses;
    uint16_t onMs;
    uint16_t gapMs;
  };

  static bool patternSpec_(BuzzerPattern pattern, uint8_t soundMode,
                           PatternSpec& spec);
  void drive_(bool active);

  bool initialized_;
  bool busy_;
  bool phaseOn_;
  bool outputActive_;
  uint8_t pulsesRemaining_;
  uint16_t onMs_;
  uint16_t gapMs_;
  uint32_t phaseStartedMs_;
};

}  // namespace att
