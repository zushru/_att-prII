#include "clock_confirmation_request.h"

#include <string.h>

namespace att {
namespace {

class JsonReader {
 public:
  JsonReader(const char* data, size_t length)
      : data_(data), length_(length), position_(0) {}

  bool decode(ClockConfirmRequest& out) {
    if (data_ == nullptr || length_ == 0 ||
        length_ > CLOCK_CONFIRM_REQUEST_MAX_BYTES) return false;
    skipSpace_();
    if (!take_('{')) return false;

    uint8_t seen = 0;
    bool first = true;
    while (true) {
      skipSpace_();
      if (peek_('}')) break;
      if (!first) {
        if (!take_(',')) return false;
        skipSpace_();
        if (peek_('}')) return false;
      }
      first = false;

      char key[32] = {};
      if (!string_(key, sizeof key) || !take_(':')) return false;
      uint8_t bit = 0;
      if (strcmp(key, "request_id") == 0) {
        bit = 0x01;
        if (!hex48_(out.requestId0, out.requestId1, out.requestId2)) return false;
      } else if (strcmp(key, "requested_utc_epoch") == 0) {
        bit = 0x02;
        uint32_t epoch = 0;
        if (!unsignedInteger_(epoch)) return false;
        out.requestedUtcEpoch = epoch;
      } else if (strcmp(key, "confirm_backward") == 0) {
        bit = 0x04;
        if (!boolean_(out.confirmBackward)) return false;
      } else if (strcmp(key, "confirm_jump") == 0) {
        bit = 0x08;
        if (!boolean_(out.confirmJump)) return false;
      } else {
        return false;
      }
      if ((seen & bit) != 0) return false;
      seen = static_cast<uint8_t>(seen | bit);
    }

    if (!take_('}') || seen != 0x0F) return false;
    skipSpace_();
    return position_ == length_;
  }

 private:
  void skipSpace_() {
    while (position_ < length_) {
      const char c = data_[position_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      ++position_;
    }
  }

  bool peek_(char c) {
    skipSpace_();
    return position_ < length_ && data_[position_] == c;
  }

  bool take_(char c) {
    skipSpace_();
    if (position_ >= length_ || data_[position_] != c) return false;
    ++position_;
    return true;
  }

  bool string_(char* out, size_t capacity) {
    skipSpace_();
    if (out == nullptr || capacity == 0 || position_ >= length_ ||
        data_[position_++] != '"') return false;
    size_t used = 0;
    while (position_ < length_) {
      const uint8_t c = static_cast<uint8_t>(data_[position_++]);
      if (c == '"') {
        out[used] = '\0';
        return true;
      }
      if (c < 0x20 || c > 0x7E || c == '\\' || used + 1 >= capacity)
        return false;
      out[used++] = static_cast<char>(c);
    }
    return false;
  }

  bool hex48_(uint16_t& out0, uint16_t& out1, uint16_t& out2) {
    skipSpace_();
    if (position_ >= length_ || data_[position_++] != '"') return false;
    uint16_t parts[3] = {0, 0, 0};
    for (size_t i = 0; i < 12; ++i) {
      if (position_ >= length_) return false;
      const char c = data_[position_++];
      uint8_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<uint8_t>(c - 'a' + 10);
      else return false;
      const size_t part = i / 4;
      parts[part] = static_cast<uint16_t>((parts[part] << 4) | digit);
    }
    if (position_ >= length_ || data_[position_++] != '"' ||
        (parts[0] == 0 && parts[1] == 0 && parts[2] == 0)) return false;
    out0 = parts[0]; out1 = parts[1]; out2 = parts[2];
    return true;
  }

  bool unsignedInteger_(uint32_t& out) {
    skipSpace_();
    if (position_ >= length_ || data_[position_] < '0' ||
        data_[position_] > '9') return false;
    if (data_[position_] == '0' && position_ + 1 < length_ &&
        data_[position_ + 1] >= '0' && data_[position_ + 1] <= '9')
      return false;
    uint32_t value = 0;
    while (position_ < length_ && data_[position_] >= '0' &&
           data_[position_] <= '9') {
      const uint32_t digit = static_cast<uint32_t>(data_[position_] - '0');
      if (value > (UINT32_MAX - digit) / 10u) return false;
      value = value * 10u + digit;
      ++position_;
    }
    if (value < 1577836800UL) return false;
    out = value;
    return true;
  }

  bool boolean_(bool& out) {
    skipSpace_();
    if (position_ + 4 <= length_ &&
        memcmp(data_ + position_, "true", 4) == 0) {
      position_ += 4;
      out = true;
      return true;
    }
    if (position_ + 5 <= length_ &&
        memcmp(data_ + position_, "false", 5) == 0) {
      position_ += 5;
      out = false;
      return true;
    }
    return false;
  }

  const char* data_;
  size_t length_;
  size_t position_;
};

}  // namespace

ClockConfirmRequestDecodeCode decodeClockConfirmRequest(
    const char* json, size_t length, ClockConfirmRequest& out) {
  if (json == nullptr || length == 0 ||
      length > CLOCK_CONFIRM_REQUEST_MAX_BYTES)
    return CCRD_INVALID_REQUEST;
  ClockConfirmRequest candidate = {0, false, false, 0, 0, 0};
  JsonReader reader(json, length);
  if (!reader.decode(candidate)) return CCRD_INVALID_REQUEST;
  out = candidate;
  return CCRD_OK;
}

}  // namespace att
