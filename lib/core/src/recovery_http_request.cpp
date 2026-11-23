#include "recovery_http_request.h"

#include <string.h>

namespace att {
namespace {

void wipe(void* value, size_t length) {
  volatile unsigned char* bytes =
      static_cast<volatile unsigned char*>(value);
  while (length-- > 0) *bytes++ = 0;
}

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

class JsonReader {
 public:
  JsonReader(const char* data, size_t length)
      : data_(data), length_(length), position_(0) {}

  bool begin() { return take_('{'); }

  bool next(char* key, size_t capacity, bool& first, bool& done) {
    skipSpace_();
    if (peek_('}')) {
      done = true;
      return true;
    }
    if (!first) {
      if (!take_(',') || peek_('}')) return false;
    }
    first = false;
    if (!string_(key, capacity) || !take_(':')) return false;
    done = false;
    return true;
  }

  bool string(char* out, size_t capacity, size_t& length) {
    return string_(out, capacity, &length);
  }

  bool uint64(uint64_t maximum, uint64_t& out) {
    skipSpace_();
    if (position_ >= length_) return false;
    const size_t start = position_;
    uint64_t value = 0;
    while (position_ < length_) {
      const char c = data_[position_];
      if (c < '0' || c > '9') break;
      const uint64_t digit = static_cast<uint64_t>(c - '0');
      if (digit > maximum || value > (maximum - digit) / 10u)
        return false;
      value = value * 10u + digit;
      ++position_;
    }
    if (position_ == start) return false;
    // JSON numbers may not contain leading zeroes except the number zero.
    if (position_ - start > 1 && data_[start] == '0') return false;
    if (position_ < length_) {
      const char next = data_[position_];
      if (next != ',' && next != '}' && next != ' ' && next != '\t' &&
          next != '\r' && next != '\n') return false;
    }
    out = value;
    return true;
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

  bool string_(char* out, size_t capacity, size_t* length = nullptr) {
    skipSpace_();
    if (out == nullptr || capacity == 0 || position_ >= length_ ||
        data_[position_++] != '"') return false;

    size_t used = 0;
    while (position_ < length_) {
      const unsigned char value =
          static_cast<unsigned char>(data_[position_++]);
      if (value == '"') {
        out[used] = '\0';
        if (length != nullptr) *length = used;
        return true;
      }
      // All accepted strings are fixed ASCII tokens. Do not implement a
      // partial JSON escape dialect for security-sensitive request fields.
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

bool decodeHex(const char* source, size_t sourceLength,
               unsigned char* destination, size_t destinationLength) {
  if (source == nullptr || destination == nullptr ||
      sourceLength != destinationLength * 2u) return false;
  unsigned char aggregate = 0;
  for (size_t i = 0; i < destinationLength; ++i) {
    const int high = hexValue(source[i * 2u]);
    const int low = hexValue(source[i * 2u + 1u]);
    if (high < 0 || low < 0) return false;
    destination[i] = static_cast<unsigned char>((high << 4) | low);
    aggregate |= destination[i];
  }
  return aggregate != 0;
}

}  // namespace

bool decodeRecoveryHttpRequest(const char* json, size_t length,
                               RecoveryHttpRequest& out) {
  if (json == nullptr || length == 0 ||
      length > RECOVERY_HTTP_REQUEST_MAX_BYTES) return false;

  RecoveryHttpRequest candidate = {};
  candidate.preview.status = RECOVERY_PREVIEW_READY;
  JsonReader reader(json, length);
  if (!reader.begin()) return false;

  enum Field : unsigned char {
    OPERATION_ID = 1u << 0,
    SOURCE_COUNT = 1u << 1,
    SOURCE_TOTAL = 1u << 2,
    RETAINED_BYTES = 1u << 3,
    MANIFEST_SHA = 1u << 4,
    PLAN_CRC = 1u << 5,
    CONFIRMATION = 1u << 6
  };
  unsigned char seen = 0;
  bool first = true;
  bool done = false;
  while (!done) {
    char key[32] = {};
    if (!reader.next(key, sizeof key, first, done)) {
      wipe(&candidate, sizeof candidate);
      return false;
    }
    if (done) break;

    unsigned char bit = 0;
    bool valid = false;
    if (strcmp(key, "operation_id") == 0) {
      char value[RECOVERY_OPERATION_ID_BYTES * 2u + 1u] = {};
      size_t valueLength = 0;
      bit = OPERATION_ID;
      valid = reader.string(value, sizeof value, valueLength) &&
          decodeHex(value, valueLength, candidate.preview.operationId,
                    sizeof candidate.preview.operationId);
      wipe(value, sizeof value);
    } else if (strcmp(key, "source_count") == 0) {
      uint64_t value = 0;
      bit = SOURCE_COUNT;
      valid = reader.uint64(RECOVERY_MAX_FILES, value) && value > 0;
      candidate.preview.sourceCount = static_cast<size_t>(value);
    } else if (strcmp(key, "source_total_bytes") == 0) {
      uint64_t value = 0;
      bit = SOURCE_TOTAL;
      valid = reader.uint64(UINT64_MAX, value);
      candidate.preview.sourceTotalBytes = value;
    } else if (strcmp(key, "retained_bytes") == 0) {
      uint64_t value = 0;
      bit = RETAINED_BYTES;
      valid = reader.uint64(UINT64_MAX, value);
      candidate.preview.retainedBytes = value;
    } else if (strcmp(key, "source_manifest_sha256") == 0) {
      char value[sizeof candidate.preview.sourceManifestSha256 * 2u + 1u] = {};
      size_t valueLength = 0;
      bit = MANIFEST_SHA;
      valid = reader.string(value, sizeof value, valueLength) &&
          decodeHex(value, valueLength,
                    candidate.preview.sourceManifestSha256,
                    sizeof candidate.preview.sourceManifestSha256);
      wipe(value, sizeof value);
    } else if (strcmp(key, "plan_crc32") == 0) {
      uint64_t value = 0;
      bit = PLAN_CRC;
      valid = reader.uint64(UINT32_MAX, value);
      candidate.preview.planCrc32 = static_cast<uint32_t>(value);
    } else if (strcmp(key, "confirmation") == 0) {
      size_t valueLength = 0;
      bit = CONFIRMATION;
      valid = reader.string(candidate.confirmation,
                            sizeof candidate.confirmation, valueLength) &&
          valueLength > 0 &&
          valueLength <= RECOVERY_CONFIRMATION_MAX_BYTES;
    } else {
      wipe(&candidate, sizeof candidate);
      return false;
    }

    if (!valid || (seen & bit) != 0) {
      wipe(&candidate, sizeof candidate);
      return false;
    }
    seen = static_cast<unsigned char>(seen | bit);
  }

  const unsigned char expected = OPERATION_ID | SOURCE_COUNT | SOURCE_TOTAL |
      RETAINED_BYTES | MANIFEST_SHA | PLAN_CRC | CONFIRMATION;
  const bool structurallyPlausible =
      candidate.preview.sourceTotalBytes >= candidate.preview.retainedBytes;
  if (seen != expected || !reader.finish() || !structurallyPlausible) {
    wipe(&candidate, sizeof candidate);
    return false;
  }

  memcpy(&out, &candidate, sizeof candidate);
  wipe(&candidate, sizeof candidate);
  return true;
}

void clearRecoveryHttpRequest(RecoveryHttpRequest& request) {
  wipe(&request, sizeof request);
}

}  // namespace att
