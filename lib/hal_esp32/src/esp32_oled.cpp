#include "esp32_oled.h"

#include <stdio.h>
#include <string.h>

namespace att {
namespace {
const uint32_t kI2cClockDuringTransferHz = 400000UL;
const uint32_t kI2cClockAfterTransferHz = 100000UL;

bool copyBoundedDisplayText(const char* source, char* destination,
                            size_t capacity, bool allowNewlines,
                            size_t& copiedLength) {
  copiedLength = 0;
  if (source == nullptr || destination == nullptr || capacity == 0)
    return false;
  for (size_t i = 0; i < capacity; ++i) {
    const uint8_t ch = static_cast<uint8_t>(source[i]);
    if (ch == 0) {
      destination[i] = '\0';
      copiedLength = i;
      return true;
    }
    if ((ch == '\n' && allowNewlines) || (ch >= 0x20u && ch <= 0x7Eu)) {
      destination[i] = static_cast<char>(ch);
      continue;
    }
    return false;
  }
  destination[capacity - 1] = '\0';
  return false;
}
}

Esp32Oled::Esp32Oled(TwoWire& sharedBus, II2cBusLock& busLock)
    : display_(kWidth, kHeight, &sharedBus, -1,
               kI2cClockDuringTransferHz, kI2cClockAfterTransferHz),
      sharedBus_(sharedBus), busLock_(busLock), initialized_(false),
      shadowValid_(false), framebufferShadow_{0}, healthStatusLine_{},
      marqueeLines_{}, marqueeLineCount_(0), marqueeActive_(false),
      marqueeClockSeen_(false), marqueeNextUpdateUs_(0),
      marqueeLastServiceUs_(0) {
  memcpy(healthStatusLine_, "ENTER CODE ON PHONE", sizeof("ENTER CODE ON PHONE"));
}

Esp32OledInitCode Esp32Oled::begin() {
  if (initialized_) return EOI_ALREADY_INITIALIZED;

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOI_BUS_LOCK_FAILED;

  // The shared bus is owned by the future board-level I2C composition root.
  // periphBegin=false prevents Adafruit from reinitializing/reconfiguring it.
  if (!display_.begin(SSD1306_SWITCHCAPVCC, kAddress, true, false)) {
    return EOI_BUFFER_ALLOC_FAILED;
  }

  initialized_ = true;
  resetFramebuffer_();
  // First frame starts with an invalid shadow, forcing a complete blank-panel
  // refresh. Later renders can update only changed page spans.
  (void)flushFrame_();
  return EOI_BUFFER_READY;
}

bool Esp32Oled::validSetupPassphrase_(const char* passphrase,
                                      size_t length) {
  if (passphrase == nullptr ||
      (length != kSetupPassphraseLength &&
       length != kPreviousHexPassphraseLength &&
       length != kLegacySetupPassphraseLength)) return false;
  if (length == kSetupPassphraseLength) {
    for (size_t i = 0; i < length; ++i) {
      const char ch = passphrase[i];
      const bool alphanumeric =
          (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9');
      if (!alphanumeric && ch != '-' && ch != '_') return false;
    }
  } else {
    for (size_t i = 0; i < length; ++i) {
      const char ch = passphrase[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
        return false;
    }
  }
  return true;
}

void Esp32Oled::resetFramebuffer_() {
  display_.clearDisplay();
  display_.setTextWrap(false);
  display_.setTextSize(1);
  display_.setTextColor(SSD1306_WHITE);
}

void Esp32Oled::cancelMarquee_() {
  memset(marqueeLines_, 0, sizeof marqueeLines_);
  marqueeLineCount_ = 0;
  marqueeActive_ = false;
  marqueeClockSeen_ = false;
  marqueeNextUpdateUs_ = 0;
  marqueeLastServiceUs_ = 0;
}

void Esp32Oled::scrubShadow_() {
  memset(framebufferShadow_, 0, sizeof framebufferShadow_);
  shadowValid_ = false;
}

bool Esp32Oled::sendBytes_(uint8_t control, const uint8_t* data,
                           size_t count) {
  if (count != 0 && data == nullptr) return false;
  sharedBus_.beginTransmission(kAddress);
  bool writeOk = sharedBus_.write(control) == 1;
  for (size_t i = 0; i < count; ++i) {
    if (sharedBus_.write(data[i]) != 1) writeOk = false;
  }
  // Always close the transaction even after a short write so Wire does not
  // retain a partially constructed message for the next caller.
  const uint8_t result = sharedBus_.endTransmission(true);
  return writeOk && result == 0;
}

bool Esp32Oled::flushFrame_(bool retainShadow) {
  uint8_t* const frame = display_.getBuffer();
  if (frame == nullptr) {
    scrubShadow_();
    return false;
  }

  bool transferred = false;
  sharedBus_.setClock(kI2cClockDuringTransferHz);
  for (uint8_t page = 0; page < (kHeight / 8u); ++page) {
    size_t first = kWidth;
    size_t last = 0;
    const size_t pageOffset = static_cast<size_t>(page) * kWidth;
    for (size_t column = 0; column < kWidth; ++column) {
      const size_t offset = pageOffset + column;
      if (!shadowValid_ || frame[offset] != framebufferShadow_[offset]) {
        if (first == kWidth) first = column;
        last = column;
      }
    }
    if (first == kWidth) continue;

    // Explicitly select horizontal addressing and a one-page rectangle. The
    // data payload then maps to a contiguous [page][column] framebuffer span.
    const uint8_t commands[] = {
        0x20, 0x00,             // memory addressing mode: horizontal
        0x21, static_cast<uint8_t>(first), static_cast<uint8_t>(last),
        0x22, page, page};
    if (!sendBytes_(0x00, commands, sizeof commands)) {
      sharedBus_.setClock(kI2cClockAfterTransferHz);
      scrubShadow_();
      return false;
    }
    transferred = true;

    size_t offset = pageOffset + first;
    size_t remaining = last - first + 1u;
    while (remaining != 0) {
      const size_t chunk = remaining > kMaxI2cDataBytes
          ? kMaxI2cDataBytes : remaining;
      if (!sendBytes_(0x40, frame + offset, chunk)) {
        // Earlier pages may already be visible. Invalidate the whole shadow
        // so the next render retries a complete, known frame.
        sharedBus_.setClock(kI2cClockAfterTransferHz);
        scrubShadow_();
        return false;
      }
      offset += chunk;
      remaining -= chunk;
    }
  }

  sharedBus_.setClock(kI2cClockAfterTransferHz);
  if (retainShadow) {
    memcpy(framebufferShadow_, frame, sizeof framebufferShadow_);
    shadowValid_ = true;
  } else {
    // Setup passwords and temporary setup codes must not be retained in the
    // differential-rendering cache, even after a successful transfer.
    scrubShadow_();
  }

#if defined(ATT_NATIVE_TEST)
  // The host Adafruit stub models panel-visible text when display() is called.
  // Hardware builds use only the bounded dirty-page I2C transfers above.
  if (transferred) display_.display();
#else
  (void)transferred;
#endif
  return true;
}

Esp32OledActionCode Esp32Oled::showSetupCredential(
    const ApCredentialResult& result, const char* passphrase,
    size_t passphraseLength) {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  cancelMarquee_();

  // A non-disclosure result or invalid credential must not leave an older
  // setup secret visible after the caller moves to another boot state.
  if (result.code != APCB_OK || !result.disclose) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_NOT_DISCLOSABLE : cleared;
  }
  if (!validSetupPassphrase_(passphrase, passphraseLength)) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL
                                             : cleared;
  }

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;

  resetFramebuffer_();
  if (passphraseLength == kSetupPassphraseLength) {
    // Eight URL-safe ASCII characters fit on one centered size-2 row.
    // On the 64-pixel panel, y=24 centers the 16-pixel glyph row vertically.
    const int16_t x = static_cast<int16_t>((kWidth - 8 * 12) / 2);
    display_.setTextSize(2);
    display_.setCursor(x, 24);
    for (size_t i = 0; i < kSetupPassphraseLength; ++i)
      display_.write(static_cast<uint8_t>(passphrase[i]));
  } else if (passphraseLength == kPreviousHexPassphraseLength) {
    // Compatibility for the earlier 16-character hexadecimal generator.
    const int16_t x = static_cast<int16_t>((kWidth - 8 * 12) / 2);
    display_.setTextSize(2);
    display_.setCursor(x, 16);
    for (size_t i = 0; i < 8; ++i)
      display_.write(static_cast<uint8_t>(passphrase[i]));
    display_.setCursor(x, 40);
    for (size_t i = 8; i < 16; ++i)
      display_.write(static_cast<uint8_t>(passphrase[i]));
  } else {
    // Backward-compatible display for a previously persisted 128-bit key.
    display_.setCursor(0, 8);
    display_.print("Wi-Fi: att");
    display_.setCursor(0, 20);
    display_.print("WPA2 password:");
    display_.setCursor(0, 36);
    for (size_t i = 0; i < 16; ++i) {
      if (i == 8) display_.write(static_cast<uint8_t>(' '));
      display_.write(static_cast<uint8_t>(passphrase[i]));
    }
    display_.setCursor(0, 44);
    for (size_t i = 16; i < kLegacySetupPassphraseLength; ++i) {
      if (i == 24) display_.write(static_cast<uint8_t>(' '));
      display_.write(static_cast<uint8_t>(passphrase[i]));
    }
  }

  // The library's display() returns void. Make the transfer request, then
  // restore a blank, size-1 framebuffer while leaving the panel image intact.
  const bool transferred = flushFrame_(false);
  resetFramebuffer_();
  return transferred ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

Esp32OledActionCode Esp32Oled::showNetworkInstructions(
    const char* ssid, const char* passphrase, size_t passphraseLength,
    const char* ipv4Address) {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  cancelMarquee_();
  if (ssid == nullptr || passphrase == nullptr || ipv4Address == nullptr ||
      strlen(ssid) == 0 || strlen(ssid) > 12 ||
      !validSetupPassphrase_(passphrase, passphraseLength)) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
  }
  // Accept only a bounded dotted-decimal IPv4 address; never render arbitrary
  // strings or rely on scanf's unbounded numeric conversion.
  const size_t addressLength = strlen(ipv4Address);
  if (addressLength < 7 || addressLength > 15) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
  }
  unsigned octets[4] = {0, 0, 0, 0};
  size_t octet = 0;
  size_t digits = 0;
  for (size_t i = 0; i <= addressLength; ++i) {
    const char ch = i == addressLength ? '\0' : ipv4Address[i];
    if (ch == '.' || ch == '\0') {
      if (digits == 0 || digits > 3 || octet >= 4 || octets[octet] > 255) {
        const Esp32OledActionCode cleared = clear();
        return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
      }
      ++octet;
      digits = 0;
    } else if (ch >= '0' && ch <= '9') {
      if (octet >= 4 || digits >= 3) {
        const Esp32OledActionCode cleared = clear();
        return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
      }
      octets[octet] = octets[octet] * 10u + static_cast<unsigned>(ch - '0');
      ++digits;
      if (octets[octet] > 255) {
        const Esp32OledActionCode cleared = clear();
        return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
      }
    } else {
      const Esp32OledActionCode cleared = clear();
      return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
    }
  }
  if (octet != 4) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_CREDENTIAL : cleared;
  }

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;
  resetFramebuffer_();
  display_.setTextSize(1);
  display_.setCursor(0, 0);
  display_.print("JOIN WI-FI: "); display_.print(ssid);
  display_.setCursor(0, 8);
  display_.print("PASSWORD:");
  // At 6 px per glyph, at most 21 characters fit on each 128 px row.
  const size_t firstLine = passphraseLength > 21 ? 21 : passphraseLength;
  display_.setCursor(0, 16);
  for (size_t i = 0; i < firstLine; ++i)
    display_.write(static_cast<uint8_t>(passphrase[i]));
  if (passphraseLength > firstLine) {
    display_.setCursor(0, 24);
    for (size_t i = firstLine; i < passphraseLength; ++i)
      display_.write(static_cast<uint8_t>(passphrase[i]));
    display_.setCursor(0, 32); display_.print("OPEN BROWSER:");
    display_.setCursor(0, 40); display_.print("http://");
    display_.setCursor(0, 48); display_.print(ipv4Address);
    display_.setCursor(0, 56); display_.print("STAY CONNECTED");
  } else {
    display_.setCursor(0, 24); display_.print("OPEN BROWSER:");
    display_.setCursor(0, 32); display_.print("http://");
    display_.setCursor(0, 40); display_.print(ipv4Address);
    display_.setCursor(0, 48); display_.print("NO INTERNET?");
    display_.setCursor(0, 56); display_.print("STAY CONNECTED");
  }
  const bool transferred = flushFrame_(false);
  resetFramebuffer_();
  return transferred ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

Esp32OledActionCode Esp32Oled::showSetupCode(const char* code,
                                             size_t codeLength,
                                             uint16_t remainingSeconds) {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  cancelMarquee_();

  bool valid = code != nullptr && codeLength == 6;
  if (valid) {
    for (size_t i = 0; i < 6; ++i) {
      if (code[i] < '0' || code[i] > '9') {
        valid = false;
        break;
      }
    }
  }
  if (!valid) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_SETUP_CODE
                                             : cleared;
  }

  if (remainingSeconds == 0 || remainingSeconds > 600) {
    const Esp32OledActionCode cleared = clear();
    return cleared == EOA_TRANSFER_REQUESTED ? EOA_INVALID_SETUP_CODE
                                             : cleared;
  }
  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;

  resetFramebuffer_();
  display_.setCursor(0, 0);
  display_.print(healthStatusLine_);
  display_.setTextSize(2);
  // Six size-2 built-in glyphs are 72 pixels wide. Center the 16-pixel
  // digit row vertically while retaining a non-secret header and expiry row.
  display_.setCursor(28, 24);
  for (size_t i = 0; i < 6; ++i)
    display_.write(static_cast<uint8_t>(code[i]));

  const unsigned minutes = (static_cast<unsigned>(remainingSeconds) + 59u) / 60u;
  char expiry[22] = {};
  snprintf(expiry, sizeof expiry, "EXPIRES IN %u MIN", minutes);
  display_.setTextSize(1);
  display_.setCursor(0, 56);
  display_.print(expiry);

  // The display API is void; retain only the panel's requested frame and
  // immediately wipe the software framebuffer. No code copy is constructed.
  const bool transferred = flushFrame_(false);
  resetFramebuffer_();
  return transferred ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

void Esp32Oled::setHealthStatusLine_(bool clockTrusted, bool sensorReady) {
  const char* line = !clockTrusted && !sensorReady ? "CLOCK! SENSOR!" :
                     !clockTrusted ? "CLOCK NOT TRUSTED" :
                     !sensorReady ? "SENSOR NOT READY" : "HEALTH OK";
  memset(healthStatusLine_, 0, sizeof healthStatusLine_);
  const size_t length = strlen(line);
  if (length < sizeof healthStatusLine_)
    memcpy(healthStatusLine_, line, length);
}

Esp32OledActionCode Esp32Oled::showHealthStatus(bool clockTrusted,
                                                bool sensorReady) {
  setHealthStatusLine_(clockTrusted, sensorReady);
  return showFault_(clockTrusted ? "CLOCK OK" : "CLOCK BAD",
                    sensorReady ? "SENSOR OK" : "SENSOR BAD");
}

Esp32OledActionCode Esp32Oled::showCodeExpired() {
  return showFault_("CODE ENDED", "WAIT NEW");
}

void Esp32Oled::renderAttendanceFrame_(const uint16_t* offsets) {
  resetFramebuffer_();
  for (uint8_t i = 0; i < marqueeLineCount_; ++i) {
    const MarqueeLine& line = marqueeLines_[i];
    if (line.text[0] == '\0') continue;
    display_.setTextSize(line.textSize);
    if (line.scroll) {
      const uint16_t offset = offsets != nullptr ? offsets[i] : line.offsetPx;
      const int16_t firstX = -static_cast<int16_t>(offset);
      const int16_t repeatX = static_cast<int16_t>(
          line.widthPx + kMarqueeGapPx - offset);
      display_.setCursor(firstX, line.y);
      display_.print(line.text);
      display_.setCursor(repeatX, line.y);
      display_.print(line.text);
    } else {
      display_.setCursor(line.x, line.y);
      display_.print(line.text);
    }
  }
}

Esp32OledActionCode Esp32Oled::showAttendanceMessage(
    const char* title, const char* detail) {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  if (title == nullptr || detail == nullptr) return EOA_INVALID_CREDENTIAL;

  char titleCopy[kMaxAttendanceTextLength + 1] = {};
  char detailCopy[kMaxAttendanceTextLength + 1] = {};
  size_t titleLength = 0;
  size_t detailLength = 0;
  if (!copyBoundedDisplayText(title, titleCopy, sizeof titleCopy, false,
                              titleLength) || titleLength == 0 ||
      !copyBoundedDisplayText(detail, detailCopy, sizeof detailCopy, true,
                              detailLength))
    return EOA_INVALID_CREDENTIAL;

  char detailLines[kMaxAttendanceDetailLines]
                  [kMaxAttendanceTextLength + 1] = {};
  size_t detailLineCount = 1;
  size_t lineLength = 0;
  for (size_t i = 0; i <= detailLength; ++i) {
    const char ch = i == detailLength ? '\0' : detailCopy[i];
    if (ch == '\n' || ch == '\0') {
      if (detailLineCount > kMaxAttendanceDetailLines)
        return EOA_INVALID_CREDENTIAL;
      detailLines[detailLineCount - 1][lineLength] = '\0';
      lineLength = 0;
      if (ch == '\n') {
        if (detailLineCount == kMaxAttendanceDetailLines)
          return EOA_INVALID_CREDENTIAL;
        ++detailLineCount;
      }
    } else {
      if (lineLength >= kMaxAttendanceTextLength)
        return EOA_INVALID_CREDENTIAL;
      detailLines[detailLineCount - 1][lineLength++] = ch;
      detailLines[detailLineCount - 1][lineLength] = '\0';
    }
  }

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;

  cancelMarquee_();
  marqueeLineCount_ = static_cast<uint8_t>(1 + detailLineCount);
  MarqueeLine& titleLine = marqueeLines_[0];
  memcpy(titleLine.text, titleCopy, titleLength + 1);
  titleLine.textSize = titleLength <= 10 ? 2 : 1;
  titleLine.widthPx = static_cast<uint16_t>(
      titleLength * 6u * titleLine.textSize);
  titleLine.x = titleLine.textSize == 2
      ? static_cast<int16_t>((kWidth - titleLine.widthPx) / 2) : 0;
  titleLine.y = titleLine.textSize == 2 ? 7 : 12;
  titleLine.scroll = titleLine.widthPx > kWidth;

  for (size_t i = 0; i < detailLineCount; ++i) {
    MarqueeLine& line = marqueeLines_[i + 1];
    const size_t length = strlen(detailLines[i]);
    memcpy(line.text, detailLines[i], length + 1);
    line.textSize = 1;
    line.widthPx = static_cast<uint16_t>(length * 6u);
    line.x = 0;
    line.y = static_cast<int16_t>(36 + i * 8);
    line.scroll = line.widthPx > kWidth;
  }

  for (size_t i = 0; i < marqueeLineCount_; ++i)
    marqueeActive_ = marqueeActive_ || marqueeLines_[i].scroll;

  renderAttendanceFrame_(nullptr);
  const bool transferred = flushFrame_();
  resetFramebuffer_();
  // Static prompts need no retained text copy. Keep only active marquee
  // content, and discard it immediately if the first transfer failed.
  if (!transferred || !marqueeActive_) cancelMarquee_();
  return transferred ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

void Esp32Oled::serviceMarquee(int64_t nowUs) {
  if (!initialized_ || !marqueeActive_ || nowUs < 0) return;

  if (!marqueeClockSeen_) {
    marqueeClockSeen_ = true;
    marqueeLastServiceUs_ = nowUs;
    marqueeNextUpdateUs_ = nowUs + kMarqueeInitialDelayUs;
    return;
  }
  if (nowUs < marqueeLastServiceUs_) {
    // A test clock or future caller may move backwards. Re-arm the initial
    // dwell instead of freezing the marquee or attempting a catch-up burst.
    marqueeLastServiceUs_ = nowUs;
    marqueeNextUpdateUs_ = nowUs + kMarqueeInitialDelayUs;
    return;
  }
  marqueeLastServiceUs_ = nowUs;
  if (nowUs < marqueeNextUpdateUs_) return;

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) {
    marqueeNextUpdateUs_ = nowUs + kMarqueeRetryIntervalUs;
    return;
  }

  uint16_t nextOffsets[kMaxMarqueeLines] = {};
  for (uint8_t i = 0; i < marqueeLineCount_; ++i) {
    const MarqueeLine& line = marqueeLines_[i];
    nextOffsets[i] = line.offsetPx;
    if (!line.scroll) continue;
    const uint16_t period = static_cast<uint16_t>(
        line.widthPx + kMarqueeGapPx);
    nextOffsets[i] = static_cast<uint16_t>(
        (line.offsetPx + kMarqueeStepPx) % period);
  }

  renderAttendanceFrame_(nextOffsets);
  const bool transferred = flushFrame_();
  resetFramebuffer_();
  if (transferred) {
    for (uint8_t i = 0; i < marqueeLineCount_; ++i)
      marqueeLines_[i].offsetPx = nextOffsets[i];
    marqueeNextUpdateUs_ = nowUs + kMarqueeStepIntervalUs;
  } else {
    // Keep the previous offset and retry the same frame later. flushFrame_
    // invalidates its shadow on a partial transfer, forcing a full repaint.
    marqueeNextUpdateUs_ = nowUs + kMarqueeRetryIntervalUs;
  }
}

Esp32OledActionCode Esp32Oled::clear() {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  cancelMarquee_();
  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;
  resetFramebuffer_();
  return flushFrame_() ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

Esp32OledActionCode Esp32Oled::showFault_(const char* firstLine,
                                               const char* secondLine) {
  if (!initialized_) return EOA_NOT_INITIALIZED;
  cancelMarquee_();
  if (firstLine == nullptr || secondLine == nullptr)
    return EOA_INVALID_CREDENTIAL;
  const size_t firstLength = strlen(firstLine);
  const size_t secondLength = strlen(secondLine);
  // At text size 2, each built-in glyph is 12 px wide: 10 chars is the
  // maximum that can fit on this 128 px panel without clipping. The two
  // 16-pixel text rows are vertically centered with an 16-pixel gap.
  if (firstLength == 0 || firstLength > 10 ||
      secondLength == 0 || secondLength > 10)
    return EOA_INVALID_CREDENTIAL;

  I2cBusLockGuard busGuard(busLock_);
  if (!busGuard.acquired()) return EOA_BUS_LOCK_FAILED;
  resetFramebuffer_();
  display_.setTextSize(2);
  const int16_t firstX =
      static_cast<int16_t>((kWidth - firstLength * 12) / 2);
  const int16_t secondX =
      static_cast<int16_t>((kWidth - secondLength * 12) / 2);
  display_.setCursor(firstX, 8);
  display_.print(firstLine);
  display_.setCursor(secondX, 40);
  display_.print(secondLine);
  const bool transferred = flushFrame_();
  resetFramebuffer_();
  return transferred ? EOA_TRANSFER_REQUESTED : EOA_TRANSFER_FAILED;
}

Esp32OledActionCode Esp32Oled::showAuthFault() {
  return showFault_("AUTH FAULT", "KEEP DATA");
}

Esp32OledActionCode Esp32Oled::showStorageFault() {
  return showFault_("DATA FAULT", "NO FORMAT");
}

Esp32OledActionCode Esp32Oled::showStorageProgress(
    Esp32OledStoragePhase phase) {
  switch (phase) {
    case EOSP_MOUNTING_READ_ONLY:
      return showFault_("MOUNT DATA", "WAIT");
    case EOSP_VALIDATING_LOG:
      return showFault_("CHECK LOG", "NO REPAIR");
    case EOSP_REPLAYING_PROJECTIONS:
      return showFault_("REPLAY LOG", "WAIT");
    default:
      return EOA_INVALID_CREDENTIAL;
  }
}

Esp32OledActionCode Esp32Oled::showConfigFault() {
  return showFault_("CFG FAULT", "KEEP DATA");
}

bool Esp32Oled::requestCredentialDisplay(const ApCredentialResult& result,
                                         const char* passphrase,
                                         size_t passphraseLength) {
  return showSetupCredential(result, passphrase, passphraseLength) ==
         EOA_TRANSFER_REQUESTED;
}

bool Esp32Oled::requestBlankFrame() {
  return clear() == EOA_TRANSFER_REQUESTED;
}

bool Esp32Oled::requestNetworkInstructions(
    const char* ssid, const char* passphrase, size_t passphraseLength,
    const char* ipv4Address) {
  return showNetworkInstructions(ssid, passphrase, passphraseLength,
                                 ipv4Address) == EOA_TRANSFER_REQUESTED;
}

}  // namespace att
