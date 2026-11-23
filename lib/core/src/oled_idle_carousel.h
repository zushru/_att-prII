#pragma once

#include <stdint.h>

namespace att {

// Idle-only OLED frame scheduler. It decides WHICH idle frame is due; it never
// draws, never touches hardware and never sees a credential. Callers must only
// poll it from the READY/idle path, after fault, setup and scan-feedback owners
// have declined the display. Any other owner that draws must call reset() so the
// next idle poll redraws the status frame instead of assuming the panel is intact.
enum OledIdleFrame : uint8_t { OIF_STATUS = 0, OIF_ADDRESS = 1 };

class OledIdleCarousel {
 public:
  static const int64_t kStatusDwellUs = 6000000LL;
  static const int64_t kAddressDwellUs = 5000000LL;

  OledIdleCarousel() { reset(); }

  void reset() {
    frame_ = OIF_STATUS;
    drawn_ = false;
    frameStartUs_ = 0;
  }

  // Returns true when `frame` must be (re)drawn now. The address frame is only
  // offered while a real AP address is available; if it disappears while shown,
  // the status frame is restored on the same poll. A backwards clock restarts
  // the dwell rather than freezing the current frame.
  bool poll(int64_t nowUs, bool addressAvailable, OledIdleFrame& frame) {
    if (nowUs < 0) return false;
    if (!drawn_) {
      frame_ = OIF_STATUS;
      drawn_ = true;
      frameStartUs_ = nowUs;
      frame = frame_;
      return true;
    }
    if (nowUs < frameStartUs_) frameStartUs_ = nowUs;
    if (frame_ == OIF_ADDRESS && !addressAvailable) {
      frame_ = OIF_STATUS;
      frameStartUs_ = nowUs;
      frame = frame_;
      return true;
    }
    const int64_t dwell =
        frame_ == OIF_STATUS ? kStatusDwellUs : kAddressDwellUs;
    if (nowUs - frameStartUs_ < dwell) return false;
    if (frame_ == OIF_STATUS) {
      if (!addressAvailable) {
        frameStartUs_ = nowUs;  // keep status; re-check after another dwell
        return false;
      }
      frame_ = OIF_ADDRESS;
    } else {
      frame_ = OIF_STATUS;
    }
    frameStartUs_ = nowUs;
    frame = frame_;
    return true;
  }

 private:
  OledIdleFrame frame_;
  bool drawn_;
  int64_t frameStartUs_;
};

}  // namespace att
