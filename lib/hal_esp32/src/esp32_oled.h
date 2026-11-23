#pragma once

#include <stddef.h>
#include <stdint.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

#include "ap_credential_display.h"
#include "i2c_bus_lock.h"

namespace att {

enum Esp32OledInitCode : uint8_t {
  EOI_BUFFER_READY,
  EOI_BUFFER_ALLOC_FAILED,
  EOI_ALREADY_INITIALIZED,
  EOI_BUS_LOCK_FAILED
};

enum Esp32OledStoragePhase : uint8_t {
  EOSP_MOUNTING_READ_ONLY,
  EOSP_VALIDATING_LOG,
  EOSP_REPLAYING_PROJECTIONS
};

enum Esp32OledActionCode : uint8_t {
  EOA_TRANSFER_REQUESTED,
  EOA_NOT_INITIALIZED,
  EOA_NOT_DISCLOSABLE,
  EOA_INVALID_CREDENTIAL,
  EOA_BUS_LOCK_FAILED,
  EOA_INVALID_SETUP_CODE,
  EOA_TRANSFER_FAILED
};

// Shared-bus SSD1306 adapter for the project's 128x64 / 0x3C OLED.
// The caller must initialize the shared TwoWire bus once (SDA 21, SCL 22)
// before begin(). This adapter never calls Wire.begin() or changes bus pins.
// The constructor and methods must be called from the serialized UI/boot task.
class Esp32Oled : public IApCredentialDisplay {
 public:
  static const uint8_t kWidth = 128;
  static const uint8_t kHeight = 64;
  static const uint8_t kAddress = 0x3C;
  static const size_t kSetupPassphraseLength = AP_CREDENTIAL_GENERATED_LENGTH;
  static const size_t kPreviousHexPassphraseLength = AP_CREDENTIAL_PREVIOUS_HEX_LENGTH;
  static const size_t kLegacySetupPassphraseLength = AP_CREDENTIAL_LEGACY_HEX_LENGTH;

  Esp32Oled(TwoWire& sharedBus, II2cBusLock& busLock);
  Esp32Oled(const Esp32Oled&) = delete;
  Esp32Oled& operator=(const Esp32Oled&) = delete;

  // EOI_BUFFER_READY means Adafruit_SSD1306 allocated its framebuffer and
  // issued initialization commands. It is not proof that a panel is present.
  Esp32OledInitCode begin();

  // Only a NEEDS_SETUP disclosure result and an exact current 8-character
  // URL-safe, previous 16-hex, or legacy 32-hex credential are accepted.
  // Current credentials render on one centered large-font row; older formats
  // retain their existing layouts. The framebuffer is wiped after the transfer
  // request; the physical panel remains visible until clear() is called.
  // Adafruit_SSD1306::display() returns void, so this reports a transfer
  // request, not a confirmed visible panel state. I2C transfer failures are
  // reported explicitly; failed updates invalidate the shadow for full retry.
  Esp32OledActionCode showSetupCredential(const ApCredentialResult& result,
                                          const char* passphrase,
                                          size_t passphraseLength);

  // Shows first-run SSID/password and the exact AP URL after the secured AP
  // has started. The password is accepted only during the first-run owner.
  Esp32OledActionCode showNetworkInstructions(const char* ssid,
                                               const char* passphrase,
                                               size_t passphraseLength,
                                               const char* ipv4Address);

  // P15 setup-code frame; separate from the E28 AP-password disclosure.
  // Accepts exactly six ASCII decimal digits, preserves leading zeroes and
  // writes directly from the caller's buffer. Invalid input requests a blank
  // frame and returns EOA_INVALID_SETUP_CODE when that request succeeds.
  // EOA_TRANSFER_REQUESTED means every emitted I2C transaction was ACKed; it
  // still cannot prove that the glass/panel visibly rendered the new frame.
  Esp32OledActionCode showSetupCode(const char* code, size_t codeLength,
                                    uint16_t remainingSeconds = 600);
  // Shows clock/sensor readiness and keeps a compact indicator above any
  // subsequently displayed setup code. This reports health, not attendance.
  Esp32OledActionCode showHealthStatus(bool clockTrusted, bool sensorReady);
  Esp32OledActionCode showCodeExpired();
  // Transient operator feedback for the live fingerprint workflow. Over-width
  // ASCII lines scroll cooperatively; fixed-layout screens use their own methods.
  Esp32OledActionCode showAttendanceMessage(const char* title,
                                            const char* detail);

  // Advances horizontal scrolling for the current attendance/status frame.
  // Call frequently from the serialized UI loop with monotonic microseconds.
  // This is cooperative and non-blocking apart from one bounded I2C refresh
  // when a scroll step is due. Other screen methods cancel the active marquee.
  void serviceMarquee(int64_t nowUs);

  // Clears the framebuffer and requests a blank frame on the panel. The
  // Adafruit API provides no write result, so hardware clearing is unverified.
  Esp32OledActionCode clear();

  // Minimal non-secret boot diagnostics; these never expose config or PIN data.
  Esp32OledActionCode showAuthFault();
  Esp32OledActionCode showStorageFault();
  // Best-effort boot progress; never authorizes or delays AP/HTTP readiness.
  Esp32OledActionCode showStorageProgress(Esp32OledStoragePhase phase);
  Esp32OledActionCode showConfigFault();

  // IApCredentialDisplay bridge used by the ordered first-provision
  // coordinator. True means only that the Adafruit transfer was requested.
  bool requestCredentialDisplay(const ApCredentialResult& result,
                                const char* passphrase,
                                size_t passphraseLength) override;
  bool requestBlankFrame() override;
  bool requestNetworkInstructions(const char* ssid, const char* passphrase,
                                  size_t passphraseLength,
                                  const char* ipv4Address) override;

  bool initialized() const { return initialized_; }

 private:
  static bool validSetupPassphrase_(const char* passphrase, size_t length);
  static const size_t kMaxAttendanceTextLength = 80;
  static const size_t kMaxAttendanceDetailLines = 3;
  static const size_t kMaxMarqueeLines = 1 + kMaxAttendanceDetailLines;
  static const uint16_t kMarqueeGapPx = 18;
  static const uint16_t kMarqueeStepPx = 3;
  static const int64_t kMarqueeInitialDelayUs = 400000LL;
  static const int64_t kMarqueeStepIntervalUs = 100000LL;
  static const int64_t kMarqueeRetryIntervalUs = 250000LL;

  struct MarqueeLine {
    char text[kMaxAttendanceTextLength + 1];
    uint16_t widthPx;
    uint16_t offsetPx;
    int16_t x;
    int16_t y;
    uint8_t textSize;
    bool scroll;
  };

  void resetFramebuffer_();
  void cancelMarquee_();
  void renderAttendanceFrame_(const uint16_t* offsets);
  void setHealthStatusLine_(bool clockTrusted, bool sensorReady);
  Esp32OledActionCode showFault_(const char* firstLine,
                                 const char* secondLine);

  static const size_t kFramebufferBytes =
      static_cast<size_t>(kWidth) * (kHeight / 8u);
  static const size_t kMaxI2cTransactionBytes = 32;
  static const size_t kMaxI2cDataBytes = kMaxI2cTransactionBytes - 1u;

  Adafruit_SSD1306 display_;
  TwoWire& sharedBus_;
  II2cBusLock& busLock_;
  bool initialized_;
  bool shadowValid_;
  uint8_t framebufferShadow_[kFramebufferBytes];
  char healthStatusLine_[22];
  MarqueeLine marqueeLines_[kMaxMarqueeLines];
  uint8_t marqueeLineCount_;
  bool marqueeActive_;
  bool marqueeClockSeen_;
  int64_t marqueeNextUpdateUs_;
  int64_t marqueeLastServiceUs_;

  // Called only while the shared I2C lock is held. It writes changed SSD1306
  // pages/column ranges in bounded transactions. Generic/secret frames can
  // request shadow invalidation so no credential/code bitmap remains in RAM.
  bool flushFrame_(bool retainShadow = true);
  bool sendBytes_(uint8_t control, const uint8_t* data, size_t count);
  void scrubShadow_();
};

}  // namespace att
