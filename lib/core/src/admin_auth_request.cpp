#include "admin_auth_request.h"

#include <string.h>

#include "api_logic.h"

namespace att {
namespace {

void wipe(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

class JsonObjectReader {
 public:
  JsonObjectReader(const char* data, size_t length)
      : data_(data), length_(length), position_(0) {}

  bool begin() { return take_('{'); }

  bool next(char* key, size_t capacity, bool& first, bool& done) {
    skipSpace_();
    if (peek_('}')) {
      done = true;
      return true;
    }
    if (!first && !take_(',')) return false;
    first = false;
    if (!string_(key, capacity) || !take_(':')) return false;
    done = false;
    return true;
  }

  bool string(char* out, size_t capacity) {
    return string_(out, capacity);
  }

  bool finish() {
    if (!take_('}')) return false;
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

  bool peek_(char value) {
    skipSpace_();
    return position_ < length_ && data_[position_] == value;
  }

  bool take_(char value) {
    skipSpace_();
    if (position_ >= length_ || data_[position_] != value) return false;
    ++position_;
    return true;
  }

  bool string_(char* out, size_t capacity) {
    skipSpace_();
    if (out == nullptr || capacity == 0 || position_ >= length_ ||
        data_[position_++] != '"') return false;

    size_t used = 0;
    while (position_ < length_) {
      const uint8_t value = static_cast<uint8_t>(data_[position_++]);
      if (value == '"') {
        out[used] = '\0';
        return true;
      }
      // These credential schemas need ASCII digits/hex only. Reject escapes
      // and controls rather than accepting a partial JSON string dialect.
      if (value < 0x20 || value > 0x7e || value == '\\' ||
          used + 1 >= capacity) return false;
      out[used++] = static_cast<char>(value);
    }
    return false;
  }

  const char* data_;
  size_t length_;
  size_t position_;
};

bool copySixDigits(JsonObjectReader& reader, char output[7]) {
  char value[7] = {};
  if (!reader.string(value, sizeof value) || !validPin(value, 6)) {
    wipe(value, sizeof value);
    return false;
  }
  memcpy(output, value, sizeof value);
  wipe(value, sizeof value);
  return true;
}

bool copyStrongPin(JsonObjectReader& reader, char output[7]) {
  char value[7] = {};
  if (!reader.string(value, sizeof value) || !validPin(value, 6) ||
      weakPin(value, 6)) {
    wipe(value, sizeof value);
    return false;
  }
  memcpy(output, value, sizeof value);
  wipe(value, sizeof value);
  return true;
}

bool copyToken(JsonObjectReader& reader, uint8_t output[16]) {
  char value[33] = {};
  if (!reader.string(value, sizeof value) || strlen(value) != 32) {
    wipe(value, sizeof value);
    return false;
  }

  uint8_t decoded[16] = {};
  uint8_t aggregate = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char high = value[i * 2];
    const char low = value[i * 2 + 1];
    const int h = high >= '0' && high <= '9' ? high - '0' :
                  high >= 'a' && high <= 'f' ? high - 'a' + 10 : -1;
    const int l = low >= '0' && low <= '9' ? low - '0' :
                  low >= 'a' && low <= 'f' ? low - 'a' + 10 : -1;
    if (h < 0 || l < 0) {
      wipe(value, sizeof value);
      wipe(decoded, sizeof decoded);
      return false;
    }
    decoded[i] = static_cast<uint8_t>((h << 4) | l);
    aggregate |= decoded[i];
  }
  wipe(value, sizeof value);
  if (aggregate == 0) {
    wipe(decoded, sizeof decoded);
    return false;
  }
  memcpy(output, decoded, sizeof decoded);
  wipe(decoded, sizeof decoded);
  return true;
}

bool expectedFields_(AdminAuthRequestKind kind, uint8_t& expected) {
  switch (kind) {
    case AAR_LOGIN: expected = 0x01; return true;
    case AAR_LOGOUT: expected = 0x02; return true;
    case AAR_PIN_CHANGE: expected = 0x0e; return true;
    default: return false;
  }
}

}  // namespace

bool decodeAdminAuthRequest(AdminAuthRequestKind kind, const char* json,
                            size_t length, AdminAuthRequest& out) {
  uint8_t expected = 0;
  if (!expectedFields_(kind, expected) || json == nullptr || length == 0 ||
      length > ADMIN_AUTH_REQUEST_MAX_BYTES) return false;

  AdminAuthRequest candidate = {};
  JsonObjectReader reader(json, length);
  if (!reader.begin()) return false;

  uint8_t seen = 0;
  bool first = true;
  bool done = false;
  while (!done) {
    char key[24] = {};
    if (!reader.next(key, sizeof key, first, done)) {
      wipe(&candidate, sizeof candidate);
      return false;
    }
    if (done) break;

    uint8_t bit = 0;
    bool valid = false;
    if (strcmp(key, "pin") == 0 && kind == AAR_LOGIN) {
      bit = 0x01;
      valid = copyStrongPin(reader, candidate.pin);
    } else if (strcmp(key, "token") == 0 &&
               (kind == AAR_LOGOUT || kind == AAR_PIN_CHANGE)) {
      bit = 0x02;
      valid = copyToken(reader, candidate.token);
    } else if (strcmp(key, "current_pin") == 0 &&
               kind == AAR_PIN_CHANGE) {
      bit = 0x04;
      valid = copySixDigits(reader, candidate.currentPin);
    } else if (strcmp(key, "new_pin") == 0 &&
               kind == AAR_PIN_CHANGE) {
      bit = 0x08;
      valid = copySixDigits(reader, candidate.newPin) &&
              !weakPin(candidate.newPin, 6);
    } else {
      wipe(&candidate, sizeof candidate);
      return false;
    }

    if (!valid || (seen & bit) != 0) {
      wipe(&candidate, sizeof candidate);
      return false;
    }
    seen = static_cast<uint8_t>(seen | bit);
  }

  if (seen != expected || !reader.finish()) {
    wipe(&candidate, sizeof candidate);
    return false;
  }
  memcpy(&out, &candidate, sizeof candidate);
  wipe(&candidate, sizeof candidate);
  return true;
}

void clearAdminAuthRequest(AdminAuthRequest& request) {
  wipe(&request, sizeof request);
}

}  // namespace att
