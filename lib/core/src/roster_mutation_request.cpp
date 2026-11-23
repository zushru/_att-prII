#include "roster_mutation_request.h"

#include <string.h>

namespace att {
namespace {

class JsonReader {
 public:
  JsonReader(const char* data, size_t length)
      : data_(data), length_(length), position_(0) {}

  bool begin() { return take_('{'); }

  bool nextKey(char* out, size_t capacity, bool& first, bool& done) {
    skipSpace_();
    if (peek_('}')) {
      done = true;
      return true;
    }
    if (!first && (!take_(',') || peek_('}'))) return false;
    first = false;
    if (!string_(out, capacity, nullptr, true) || !take_(':')) return false;
    done = false;
    return true;
  }

  bool readOperation(RosterMutationOperation& out) {
    char value[12] = {};
    size_t length = 0;
    if (!string_(value, sizeof value, &length, true)) return false;
    if (length == 3 && memcmp(value, "set", 3) == 0) out = RMO_SET;
    else if (length == 6 && memcmp(value, "create", 6) == 0) out = RMO_CREATE;
    else if (length == 8 && memcmp(value, "activate", 8) == 0)
      out = RMO_ACTIVATE;
    else if (length == 10 && memcmp(value, "deactivate", 10) == 0)
      out = RMO_DEACTIVATE;
    else return false;
    return true;
  }

  bool readName(char out[ROSTER_NAME_MAX + 1], size_t& outLength) {
    return string_(out, ROSTER_NAME_MAX + 1, &outLength, false);
  }

  bool readId(uint16_t& out) {
    skipSpace_();
    if (position_ >= length_ || data_[position_] < '0' ||
        data_[position_] > '9') return false;
    const size_t start = position_;
    uint16_t value = 0;
    while (position_ < length_ && data_[position_] >= '0' &&
           data_[position_] <= '9') {
      const uint16_t digit = static_cast<uint16_t>(data_[position_] - '0');
      if (digit > MAX_ID || value > (MAX_ID - digit) / 10u) return false;
      value = static_cast<uint16_t>(value * 10u + digit);
      ++position_;
    }
    if (position_ - start > 1 && data_[start] == '0') return false;
    if (value == 0 || value > MAX_ID) return false;
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

  bool hex4_(uint16_t& value) {
    if (length_ - position_ < 4) return false;
    uint16_t parsed = 0;
    for (size_t i = 0; i < 4; ++i) {
      const char c = data_[position_++];
      uint8_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<uint8_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<uint8_t>(c - 'A' + 10);
      else return false;
      parsed = static_cast<uint16_t>((parsed << 4) | digit);
    }
    value = parsed;
    return true;
  }

  bool appendCodePoint_(uint32_t cp, char* out, size_t capacity,
                        size_t& used) {
    uint8_t encoded[4];
    size_t count = 0;
    if (cp <= 0x7Fu) {
      encoded[count++] = static_cast<uint8_t>(cp);
    } else if (cp <= 0x7FFu) {
      encoded[count++] = static_cast<uint8_t>(0xC0u | (cp >> 6));
      encoded[count++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
      encoded[count++] = static_cast<uint8_t>(0xE0u | (cp >> 12));
      encoded[count++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
      encoded[count++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0x10FFFFu) {
      encoded[count++] = static_cast<uint8_t>(0xF0u | (cp >> 18));
      encoded[count++] = static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu));
      encoded[count++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
      encoded[count++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    } else {
      return false;
    }
    if (count >= capacity || used > capacity - count - 1) return false;
    memcpy(out + used, encoded, count);
    used += count;
    return true;
  }

  bool string_(char* out, size_t capacity, size_t* outLength,
               bool asciiOnly) {
    skipSpace_();
    if (out == nullptr || capacity == 0 || position_ >= length_ ||
        data_[position_++] != '"') return false;
    size_t used = 0;
    while (position_ < length_) {
      const uint8_t raw = static_cast<uint8_t>(data_[position_++]);
      // Raw UTF-8 bytes must remain byte-for-byte intact; RosterStore performs
      // the complete UTF-8 validation after decoding the name field.
      if (raw >= 0x80u) {
        if (asciiOnly || used + 1 >= capacity) return false;
        out[used++] = static_cast<char>(raw);
        continue;
      }
      uint32_t cp = raw;
      if (cp == '"') {
        out[used] = '\0';
        if (outLength != nullptr) *outLength = used;
        return true;
      }
      if (cp == '\\') {
        if (position_ >= length_) return false;
        const char escaped = data_[position_++];
        switch (escaped) {
          case '"': cp = '"'; break;
          case '\\': cp = '\\'; break;
          case '/': cp = '/'; break;
          case 'b': cp = '\b'; break;
          case 'f': cp = '\f'; break;
          case 'n': cp = '\n'; break;
          case 'r': cp = '\r'; break;
          case 't': cp = '\t'; break;
          case 'u': {
            uint16_t first = 0;
            if (!hex4_(first)) return false;
            if (first >= 0xD800u && first <= 0xDBFFu) {
              if (length_ - position_ < 6 || data_[position_] != '\\' ||
                  data_[position_ + 1] != 'u') return false;
              position_ += 2;
              uint16_t second = 0;
              if (!hex4_(second) || second < 0xDC00u || second > 0xDFFFu)
                return false;
              cp = 0x10000u +
                  ((static_cast<uint32_t>(first) - 0xD800u) << 10) +
                  (static_cast<uint32_t>(second) - 0xDC00u);
            } else {
              if (first >= 0xDC00u && first <= 0xDFFFu) return false;
              cp = first;
            }
            break;
          }
          default: return false;
        }
      }
      if (cp < 0x20u || cp == 0x7Fu || (asciiOnly && cp > 0x7Eu))
        return false;
      if (!appendCodePoint_(cp, out, capacity, used)) return false;
    }
    return false;
  }

  const char* data_;
  size_t length_;
  size_t position_;
};

}  // namespace

bool decodeRosterMutationRequest(const char* json, size_t length,
                                 RosterMutationRequest& out) {
  if (json == nullptr || length == 0 ||
      length > ROSTER_MUTATION_REQUEST_MAX_BYTES) return false;

  RosterMutationRequest candidate = {};
  JsonReader reader(json, length);
  if (!reader.begin()) return false;

  enum Field : uint8_t { OPERATION = 1u, ID = 2u, NAME = 4u };
  uint8_t seen = 0;
  bool first = true;
  bool done = false;
  while (!done) {
    char key[16] = {};
    if (!reader.nextKey(key, sizeof key, first, done)) return false;
    if (done) break;

    uint8_t bit = 0;
    bool valid = false;
    if (strcmp(key, "operation") == 0) {
      bit = OPERATION;
      valid = reader.readOperation(candidate.operation);
    } else if (strcmp(key, "id") == 0) {
      bit = ID;
      valid = reader.readId(candidate.id);
    } else if (strcmp(key, "name") == 0) {
      bit = NAME;
      valid = reader.readName(candidate.name, candidate.nameLength);
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint8_t>(seen | bit);
  }

  if (!reader.finish() || (seen & (OPERATION | ID)) != (OPERATION | ID))
    return false;
  if (candidate.operation == RMO_SET || candidate.operation == RMO_CREATE) {
    if ((seen & NAME) == 0 ||
        !RosterStore::validName(candidate.name, candidate.nameLength))
      return false;
  } else if ((seen & NAME) != 0) {
    return false;
  }

  out = candidate;
  return true;
}

}  // namespace att
