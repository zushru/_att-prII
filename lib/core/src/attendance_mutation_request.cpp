#include "attendance_mutation_request.h"

#include <string.h>

namespace att {
namespace {

class Reader {
 public:
  Reader(const char* data, size_t length)
      : data_(data), length_(length), pos_(0) {}

  bool begin() { return take_('{'); }
  bool next(char* key, size_t capacity, bool& first, bool& done) {
    skip_();
    if (peek_('}')) { done = true; return true; }
    if (!first && (!take_(',') || peek_('}'))) return false;
    first = false;
    return string_(key, capacity) && take_(':') ? (done = false, true) : false;
  }
  bool number(uint32_t maximum, size_t maxDigits, uint32_t& out) {
    skip_();
    if (pos_ >= length_ || data_[pos_] < '0' || data_[pos_] > '9')
      return false;
    const size_t start = pos_;
    uint32_t value = 0;
    while (pos_ < length_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
      if (pos_ - start >= maxDigits) return false;
      const uint32_t digit = static_cast<uint32_t>(data_[pos_] - '0');
      if (digit > maximum || value > (maximum - digit) / 10u) return false;
      value = value * 10u + digit;
      ++pos_;
    }
    if (pos_ - start > 1 && data_[start] == '0') return false;
    out = value;
    return true;
  }
  bool word(char* out, size_t capacity) { return string_(out, capacity); }
  bool boolean(bool& out) {
    skip_();
    if (pos_ + 4u <= length_ && memcmp(data_ + pos_, "true", 4) == 0) {
      pos_ += 4u;
      out = true;
      return true;
    }
    if (pos_ + 5u <= length_ && memcmp(data_ + pos_, "false", 5) == 0) {
      pos_ += 5u;
      out = false;
      return true;
    }
    return false;
  }
  bool requestId(AttendanceMutationRequestId& out) {
    char text[17] = {};
    if (!string_(text, sizeof text) || strlen(text) != 16) return false;
    uint16_t words[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 16; ++i) {
      const char c = text[i];
      uint8_t nibble = 0;
      if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        nibble = static_cast<uint8_t>(c - 'a' + 10);
      else return false;  // canonical lowercase hex only
      words[i / 4] = static_cast<uint16_t>(
          (words[i / 4] << 4) | nibble);
    }
    if (words[0] == 0 && words[1] == 0 &&
        words[2] == 0 && words[3] == 0) return false;
    for (size_t i = 0; i < 4; ++i) out.words[i] = words[i];
    return true;
  }
  bool finish() { return take_('}') && atEnd_(); }

 private:
  void skip_() {
    while (pos_ < length_) {
      const char c = data_[pos_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      ++pos_;
    }
  }
  bool peek_(char c) {
    skip_();
    return pos_ < length_ && data_[pos_] == c;
  }
  bool take_(char c) {
    skip_();
    if (pos_ >= length_ || data_[pos_] != c) return false;
    ++pos_;
    return true;
  }
  bool atEnd_() {
    skip_();
    return pos_ == length_;
  }
  bool string_(char* out, size_t capacity) {
    skip_();
    if (!out || capacity == 0 || pos_ >= length_ || data_[pos_++] != '"')
      return false;
    size_t used = 0;
    while (pos_ < length_) {
      const unsigned char c = static_cast<unsigned char>(data_[pos_++]);
      if (c == '"') {
        out[used] = '\0';
        return true;
      }
      // Keys and enum values are deliberately ASCII literals. Escapes,
      // control bytes and non-ASCII spellings are unnecessary and rejected.
      if (c < 0x21u || c > 0x7Eu || c == '\\' || used + 1 >= capacity)
        return false;
      out[used++] = static_cast<char>(c);
    }
    return false;
  }

  const char* data_;
  size_t length_;
  size_t pos_;
};

bool inputOk(const char* json, size_t length) {
  return json != nullptr && length != 0 &&
      length <= ATTENDANCE_MUTATION_REQUEST_MAX_BYTES;
}

}  // namespace

const char* adminMutationReasonName(AdminMutationReason reason) {
  switch (reason) {
    case AMR_DATA_ENTRY_ERROR: return "data_entry_error";
    case AMR_SOURCE_RECORD: return "source_record";
    case AMR_STAFF_CONFIRMATION: return "staff_confirmation";
    case AMR_ADMINISTRATIVE_APPROVAL: return "administrative_approval";
    case AMR_OTHER_DOCUMENTED: return "other_documented";
    case AMR_NONE: default: return nullptr;
  }
}

bool parseAdminMutationReason(const char* value, AdminMutationReason& out) {
  if (value == nullptr) return false;
  for (uint16_t candidate = AMR_DATA_ENTRY_ERROR;
       candidate <= AMR_OTHER_DOCUMENTED; ++candidate) {
    const AdminMutationReason reason =
        static_cast<AdminMutationReason>(candidate);
    const char* name = adminMutationReasonName(reason);
    if (name != nullptr && strcmp(value, name) == 0) {
      out = reason;
      return true;
    }
  }
  return false;
}

bool decodeMarkMutationRequest(const char* json, size_t length, MarkReq& out,
                               AttendanceMutationRequestId& requestId,
                               AdminMutationReason* reason) {
  if (!inputOk(json, length)) return false;
  Reader reader(json, length);
  if (!reader.begin()) return false;
  enum : uint8_t { REQUEST = 1u, ID = 2u, DAY = 4u, SESSION = 8u,
                   STATUS = 16u, REASON = 32u };
  uint8_t seen = 0;
  bool first = true, done = false;
  MarkReq candidate = {};
  AttendanceMutationRequestId candidateId = {{0, 0, 0, 0}};
  AdminMutationReason candidateReason = AMR_NONE;
  while (!done) {
    char key[16] = {};
    if (!reader.next(key, sizeof key, first, done)) return false;
    if (done) break;
    uint8_t bit = 0;
    bool valid = false;
    uint32_t value = 0;
    if (strcmp(key, "request_id") == 0) {
      bit = REQUEST;
      valid = reader.requestId(candidateId);
    } else if (strcmp(key, "student_id") == 0) {
      bit = ID;
      valid = reader.number(MAX_ID, 3, value) && value >= 1;
      if (valid) candidate.id = static_cast<uint16_t>(value);
    } else if (strcmp(key, "day") == 0) {
      bit = DAY;
      valid = reader.number(MAX_SUPPORTED_DAY, 5, value);
      if (valid) candidate.day = static_cast<uint16_t>(value);
    } else if (strcmp(key, "session") == 0) {
      bit = SESSION;
      valid = reader.number(1, 1, value);
      if (valid) candidate.session = static_cast<uint8_t>(value);
    } else if (strcmp(key, "status") == 0) {
      bit = STATUS;
      char status[12] = {};
      valid = reader.word(status, sizeof status);
      if (valid) {
        if (strcmp(status, "clear") == 0) candidate.status = ST_CLEAR;
        else if (strcmp(status, "present") == 0) candidate.status = ST_PRESENT;
        else if (strcmp(status, "late") == 0) candidate.status = ST_LATE;
        else if (strcmp(status, "excused") == 0) candidate.status = ST_EXCUSED;
        else valid = false;
      }
    } else if (strcmp(key, "reason") == 0) {
      bit = REASON;
      char value[24] = {};
      valid = reader.word(value, sizeof value) &&
          parseAdminMutationReason(value, candidateReason);
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint8_t>(seen | bit);
  }
  const uint8_t required = REQUEST | ID | DAY | SESSION | STATUS;
  if (!reader.finish() || (seen & required) != required ||
      (seen & (uint8_t)~(required | REASON)) != 0) return false;
  out = candidate;
  requestId = candidateId;
  if (reason != nullptr) *reason = candidateReason;
  return true;
}

bool decodeTodayStatusMutationRequest(
    const char* json, size_t length, TodayStatusMutationRequest& out,
    AttendanceMutationRequestId& requestId) {
  if (!inputOk(json, length)) return false;
  Reader reader(json, length);
  if (!reader.begin()) return false;
  enum : uint16_t {
    REQUEST = 1u, ID = 2u, DAY = 4u, SESSION = 8u, STATUS = 16u,
    REVISION = 32u, RECORDS = 64u, GEN_HIGH = 128u, GEN_LOW = 256u
  };
  uint16_t seen = 0;
  bool first = true, done = false;
  TodayStatusMutationRequest candidate = {};
  AttendanceMutationRequestId candidateId = {{0, 0, 0, 0}};
  while (!done) {
    char key[24] = {};
    if (!reader.next(key, sizeof key, first, done)) return false;
    if (done) break;
    uint16_t bit = 0;
    uint32_t value = 0;
    bool valid = false;
    if (strcmp(key, "request_id") == 0) {
      bit = REQUEST;
      valid = reader.requestId(candidateId);
    } else if (strcmp(key, "student_id") == 0) {
      bit = ID;
      valid = reader.number(MAX_ID, 3, value) && value >= 1;
      if (valid) candidate.studentId = static_cast<uint16_t>(value);
    } else if (strcmp(key, "day") == 0) {
      bit = DAY;
      valid = reader.number(MAX_SUPPORTED_DAY, 5, value);
      if (valid) candidate.day = static_cast<uint16_t>(value);
    } else if (strcmp(key, "session") == 0) {
      bit = SESSION;
      valid = reader.number(1, 1, value);
      if (valid) candidate.session = static_cast<uint8_t>(value);
    } else if (strcmp(key, "status") == 0) {
      bit = STATUS;
      char status[12] = {};
      valid = reader.word(status, sizeof status);
      if (valid) {
        if (strcmp(status, "present") == 0) candidate.status = ST_PRESENT;
        else if (strcmp(status, "pending") == 0) candidate.status = ST_PENDING;
        else if (strcmp(status, "excused") == 0) candidate.status = ST_EXCUSED;
        else valid = false;
      }
    } else if (strcmp(key, "expected_revision") == 0) {
      bit = REVISION;
      valid = reader.number(UINT32_MAX, 10, value);
      if (valid) candidate.expectedRevision = value;
    } else if (strcmp(key, "expected_records") == 0) {
      bit = RECORDS;
      valid = reader.number(ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS, 10, value);
      if (valid) candidate.expectedRecords = value;
    } else if (strcmp(key, "generation_high") == 0) {
      bit = GEN_HIGH;
      valid = reader.number(UINT32_MAX, 10, value);
      if (valid) candidate.generationHigh = value;
    } else if (strcmp(key, "generation_low") == 0) {
      bit = GEN_LOW;
      valid = reader.number(UINT32_MAX, 10, value);
      if (valid) candidate.generationLow = value;
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint16_t>(seen | bit);
  }
  const uint16_t required = REQUEST | ID | DAY | SESSION | STATUS |
      REVISION | RECORDS | GEN_HIGH | GEN_LOW;
  if (!reader.finish() || seen != required ||
      (candidate.generationHigh == 0 && candidate.generationLow == 0))
    return false;
  out = candidate;
  requestId = candidateId;
  return true;
}

bool decodeAutomaticNoClassReviewRequest(
    const char* json, size_t length, AutomaticNoClassReviewRequest& out,
    AttendanceMutationRequestId& requestId) {
  if (!inputOk(json, length)) return false;
  Reader reader(json, length);
  if (!reader.begin()) return false;
  enum : uint16_t {
    REQUEST = 1u, DAY = 2u, SESSION = 4u, ACTION = 8u,
    AUTO_SEQUENCE = 16u, RECORDS = 32u, GEN_HIGH = 64u, GEN_LOW = 128u
  };
  uint16_t seen = 0;
  bool first = true, done = false;
  AutomaticNoClassReviewRequest candidate = {};
  AttendanceMutationRequestId candidateId = {{0, 0, 0, 0}};
  while (!done) {
    char key[24] = {};
    if (!reader.next(key, sizeof key, first, done)) return false;
    if (done) break;
    uint16_t bit = 0;
    uint32_t value = 0;
    bool valid = false;
    if (strcmp(key, "request_id") == 0) {
      bit = REQUEST;
      valid = reader.requestId(candidateId);
    } else if (strcmp(key, "day") == 0) {
      bit = DAY;
      valid = reader.number(MAX_SUPPORTED_DAY, 5, value);
      if (valid) candidate.day = static_cast<uint16_t>(value);
    } else if (strcmp(key, "session") == 0) {
      bit = SESSION;
      valid = reader.number(1, 1, value);
      if (valid) candidate.session = static_cast<uint8_t>(value);
    } else if (strcmp(key, "action") == 0) {
      bit = ACTION;
      char action[12] = {};
      valid = reader.word(action, sizeof action);
      if (valid) {
        if (strcmp(action, "confirm") == 0) candidate.action = ANCR_CONFIRM;
        else if (strcmp(action, "undo") == 0) candidate.action = ANCR_UNDO;
        else valid = false;
      }
    } else if (strcmp(key, "expected_auto_sequence") == 0) {
      bit = AUTO_SEQUENCE;
      valid = reader.number(UINT32_MAX - 1u, 10, value) && value != 0;
      if (valid) candidate.expectedAutoSequence = value;
    } else if (strcmp(key, "expected_records") == 0) {
      bit = RECORDS;
      valid = reader.number(ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS, 10, value);
      if (valid) candidate.expectedRecords = value;
    } else if (strcmp(key, "generation_high") == 0) {
      bit = GEN_HIGH;
      valid = reader.number(UINT32_MAX, 10, value);
      if (valid) candidate.generationHigh = value;
    } else if (strcmp(key, "generation_low") == 0) {
      bit = GEN_LOW;
      valid = reader.number(UINT32_MAX, 10, value);
      if (valid) candidate.generationLow = value;
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint16_t>(seen | bit);
  }
  const uint16_t required = REQUEST | DAY | SESSION | ACTION |
      AUTO_SEQUENCE | RECORDS | GEN_HIGH | GEN_LOW;
  if (!reader.finish() || seen != required ||
      candidate.expectedAutoSequence == 0 ||
      candidate.expectedAutoSequence > candidate.expectedRecords ||
      candidate.expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
      (candidate.generationHigh == 0 && candidate.generationLow == 0))
    return false;
  out = candidate;
  requestId = candidateId;
  return true;
}

bool decodeHistoricalMarkMutationRequest(
    const char* json, size_t length, HistoricalMarkMutationRequest& out) {
  if (!inputOk(json, length)) return false;
  Reader reader(json, length);
  if (!reader.begin()) return false;
  enum : uint16_t {
    REQUEST = 1u, ID = 2u, DAY = 4u, SESSION = 8u, STATUS = 16u,
    REASON = 32u, RECORDS = 64u, GEN_HIGH = 128u, GEN_LOW = 256u,
    OVERRIDE = 512u
  };
  uint16_t seen = 0;
  bool first = true, done = false;
  HistoricalMarkMutationRequest candidate = {};
  candidate.reason = AMR_NONE;
  while (!done) {
    char key[20] = {};
    if (!reader.next(key, sizeof key, first, done)) return false;
    if (done) break;
    uint16_t bit = 0;
    bool valid = false;
    uint32_t value = 0;
    if (strcmp(key, "request_id") == 0) {
      bit = REQUEST;
      valid = reader.requestId(candidate.requestId);
    } else if (strcmp(key, "student_id") == 0) {
      bit = ID;
      valid = reader.number(MAX_ID, 3, value) && value >= 1;
      if (valid) candidate.mark.id = static_cast<uint16_t>(value);
    } else if (strcmp(key, "day") == 0) {
      bit = DAY;
      valid = reader.number(MAX_SUPPORTED_DAY, 5, value);
      if (valid) candidate.mark.day = static_cast<uint16_t>(value);
    } else if (strcmp(key, "session") == 0) {
      bit = SESSION;
      valid = reader.number(1, 1, value);
      if (valid) candidate.mark.session = static_cast<uint8_t>(value);
    } else if (strcmp(key, "status") == 0) {
      bit = STATUS;
      char status[12] = {};
      valid = reader.word(status, sizeof status);
      if (valid) {
        if (strcmp(status, "present") == 0) candidate.mark.status = ST_PRESENT;
        else if (strcmp(status, "late") == 0) candidate.mark.status = ST_LATE;
        else if (strcmp(status, "excused") == 0) candidate.mark.status = ST_EXCUSED;
        else valid = false;  // historical backfill never clears a mark
      }
    } else if (strcmp(key, "reason") == 0) {
      bit = REASON;
      char reason[24] = {};
      valid = reader.word(reason, sizeof reason) &&
          parseAdminMutationReason(reason, candidate.reason);
    } else if (strcmp(key, "expected_records") == 0) {
      bit = RECORDS;
      valid = reader.number(UINT32_MAX, 10, candidate.expectedRecords);
    } else if (strcmp(key, "generation_high") == 0) {
      bit = GEN_HIGH;
      valid = reader.number(UINT32_MAX, 10, candidate.generationHigh);
    } else if (strcmp(key, "generation_low") == 0) {
      bit = GEN_LOW;
      valid = reader.number(UINT32_MAX, 10, candidate.generationLow);
    } else if (strcmp(key, "override_existing") == 0) {
      bit = OVERRIDE;
      valid = reader.boolean(candidate.overrideExisting);
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint16_t>(seen | bit);
  }
  const uint16_t required = REQUEST | ID | DAY | SESSION | STATUS | REASON |
      RECORDS | GEN_HIGH | GEN_LOW;
  if (!reader.finish() || (seen & required) != required ||
      (seen & static_cast<uint16_t>(~(required | OVERRIDE))) != 0 ||
      candidate.expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
      (candidate.generationHigh == 0 && candidate.generationLow == 0) ||
      candidate.reason < AMR_DATA_ENTRY_ERROR ||
      candidate.reason > AMR_OTHER_DOCUMENTED) return false;
  out = candidate;
  return true;
}

bool decodeNoClassMutationRequest(const char* json, size_t length,
                                  NoClassReq& out,
                                  AttendanceMutationRequestId& requestId,
                                  AdminMutationReason* reason) {
  if (!inputOk(json, length)) return false;
  Reader reader(json, length);
  if (!reader.begin()) return false;
  enum : uint8_t { REQUEST = 1u, DAY = 2u, SCOPE = 4u,
                   OPERATION = 8u, SESSION = 16u, REASON = 32u };
  uint8_t seen = 0;
  bool first = true, done = false;
  uint32_t day = 0, session = 0;
  bool wholeDay = false, set = false;
  AttendanceMutationRequestId candidateId = {{0, 0, 0, 0}};
  AdminMutationReason candidateReason = AMR_NONE;
  while (!done) {
    char key[16] = {};
    if (!reader.next(key, sizeof key, first, done)) return false;
    if (done) break;
    uint8_t bit = 0;
    bool valid = false;
    if (strcmp(key, "request_id") == 0) {
      bit = REQUEST;
      valid = reader.requestId(candidateId);
    } else if (strcmp(key, "day") == 0) {
      bit = DAY;
      valid = reader.number(MAX_SUPPORTED_DAY, 5, day);
    } else if (strcmp(key, "scope") == 0) {
      bit = SCOPE;
      char scope[10] = {};
      valid = reader.word(scope, sizeof scope);
      if (valid) {
        if (strcmp(scope, "day") == 0) wholeDay = true;
        else if (strcmp(scope, "session") == 0) wholeDay = false;
        else valid = false;
      }
    } else if (strcmp(key, "operation") == 0) {
      bit = OPERATION;
      char operation[8] = {};
      valid = reader.word(operation, sizeof operation);
      if (valid) {
        if (strcmp(operation, "set") == 0) set = true;
        else if (strcmp(operation, "clear") == 0) set = false;
        else valid = false;
      }
    } else if (strcmp(key, "session") == 0) {
      bit = SESSION;
      valid = reader.number(1, 1, session);
    } else if (strcmp(key, "reason") == 0) {
      bit = REASON;
      char value[24] = {};
      valid = reader.word(value, sizeof value) &&
          parseAdminMutationReason(value, candidateReason);
    } else {
      return false;
    }
    if (!valid || (seen & bit) != 0) return false;
    seen = static_cast<uint8_t>(seen | bit);
  }
  const uint8_t required = REQUEST | DAY | SCOPE | OPERATION;
  if (!reader.finish() || (seen & required) != required ||
      (seen & (uint8_t)~(required | SESSION | REASON)) != 0) return false;
  if (wholeDay ? ((seen & SESSION) != 0) : ((seen & SESSION) == 0))
    return false;
  NoClassReq candidate = {
      static_cast<uint16_t>(day), static_cast<uint8_t>(session), wholeDay, set};
  out = candidate;
  requestId = candidateId;
  if (reason != nullptr) *reason = candidateReason;
  return true;
}

}  // namespace att
