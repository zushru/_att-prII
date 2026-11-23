// HTTP adapters are production-only; native tests exercise their core boundaries.
#if defined(ESP_PLATFORM)
#include "esp32_admin_roster_http_handler.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <Arduino.h>
#include "event_codec.h"
#include "time_rules.h"
#include "closure_engine.h"
#include "admin_session_fingerprint.h"
#include "admin_mutation_audit.h"
namespace att {
static_assert(ATTENDANCE_MUTATION_REQUEST_MAX_BYTES ==
              BOUNDED_JSON_BODY_MAX_BYTES,
              "attendance JSON parser and HTTP body limits must match");
static_assert(ROSTER_MUTATION_REQUEST_MAX_BYTES ==
              BOUNDED_JSON_BODY_MAX_BYTES,
              "roster JSON parser and HTTP body limits must match");
namespace {
const char kRosterUri[] = "/api/v1/roster";
const char kTodayUri[] = "/api/v1/today";
const char kTodayStatusUri[] = "/api/v1/today/status";
const char kTodayNoClassUri[] = "/api/v1/today/no-class";
const char kTodayNoClassReviewUri[] = "/api/v1/today/no-class-review";
const char kPendingAutoNoClassUri[] = "/api/v1/automatic-no-class-pending";
const uint8_t kPendingAutoNoClassPageSize = 16;
const char kTimetableUri[] = "/api/v1/timetable";
const char kDayScansUri[] = "/api/v1/day-scans";
const char kDaySummaryUri[] = "/api/v1/day-summary";
const char kDayAdjustmentsUri[] = "/api/v1/day-adjustments";
const char kHistoricalGridUri[] = "/api/v1/historical-grid";
const char kHistoricalMarkUri[] = "/api/v1/historical-mark";
const uint8_t kRosterPageSize = 16;
const uint8_t kDayScanPageSize = 16;
const uint8_t kDaySummaryPageSize = 16;
const uint8_t kTodayPageSize = 7;
const uint8_t kDaySummaryReadBatchRecords = 16;
const uint16_t kDaySummaryRecordBudget = 128;
const uint8_t kDayScanReadBatchRecords = 16;
const uint16_t kDayScanRecordBudget = 128;
const uint8_t kDayAdjustmentPageSize = 8;
const uint8_t kDayAdjustmentReadBatchRecords = 16;
const uint16_t kDayAdjustmentRecordBudget = 128;
const uint8_t kHistoricalGridPageSize = 8;
const uint8_t kHistoricalReadBatchRecords = 32;
const uint16_t kHistoricalRecordBudget = 512;
const uint32_t kResponseWriteTimeoutMs = 250;
void wipe(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}
bool parseDecimal(const String& text, uint32_t maximum, size_t maxDigits,
                  uint32_t& out) {
  if (text.length() == 0 || text.length() > maxDigits) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (digit > maximum || value > (maximum - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}
int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}
bool decodeBearer(const String& header, uint8_t token[16]) {
  static const char prefix[] = "Bearer ";
  if (token == nullptr || header.length() != 39) return false;
  for (size_t i = 0; i < sizeof(prefix) - 1; ++i)
    if (header[i] != prefix[i]) return false;
  for (size_t i = 0; i < 16; ++i) {
    const int high = hexValue(header[7 + i * 2]);
    const int low = hexValue(header[8 + i * 2]);
    if (high < 0 || low < 0) { wipe(token, 16); return false; }
    token[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}
const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 423: return "Locked";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}
}
Esp32AdminRosterHttpHandler::Esp32AdminRosterHttpHandler(
    AdminSessionBoundary& sessions, LogStore& log, MonthSet& monthData,
    RosterStore& roster, const Config& config,
    HealthFsm& health,
    MonotonicSecondsSource monotonicSeconds, bool& applicationFaulted,
    RosterEventWriter* writer, Ds3231Clock* rtcClock,
    ClockHealthOwner* clockHealth, bool* storageWritesAuthorized,
    AttendanceEventWriter* attendanceWriter, const LogIndex* logIndex)
    : sessions_(sessions), log_(log), monthData_(monthData), roster_(roster),
      dayView_(), todayRevision_{}, todayScanEpoch_{},
      todayLastStatusEpoch_{}, todayLastStatusSequence_{},
      dayCacheDay_(0), dayCacheSession_(0), dayCacheNextRecord_(0),
      dayCacheRecordCount_(0), dayCacheGeneration_(0),
      dayCacheInitialized_(false), dayCacheReady_(false),
      historicalViews_{}, historicalColumns_{}, historicalFromDay_(-1),
      historicalToDay_(-1), historicalNextRecord_(0),
      historicalRecordCount_(0), historicalGeneration_(0),
      historicalColumnCount_(0), historicalInitialized_(false),
      historicalReady_(false),
      config_(config), health_(health),
      monotonicSeconds_(monotonicSeconds), applicationFaulted_(applicationFaulted),
      writer_(writer), rtcClock_(rtcClock), clockHealth_(clockHealth),
      storageWritesAuthorized_(storageWritesAuthorized),
      attendanceWriter_(attendanceWriter), logIndex_(logIndex), mutationBody_(),
      response_(), responseLength_(0) {}
bool Esp32AdminRosterHttpHandler::canHandle(
    WebServer& server, HTTPMethod method, const String& uri) {
  (void)server;
  if (method == HTTP_POST && uri == kRosterUri) return writer_ != nullptr;
  if (method == HTTP_POST && (uri == kTodayStatusUri ||
                              uri == kTodayNoClassUri ||
                              uri == kTodayNoClassReviewUri))
    return attendanceWriter_ != nullptr;
  if (method == HTTP_POST &&
      (uri == "/api/v1/mark" || uri == "/api/v1/no-class" ||
       uri == kHistoricalMarkUri))
    return attendanceWriter_ != nullptr;
  return method == HTTP_GET &&
      (uri == kRosterUri || uri == kTodayUri || uri == kPendingAutoNoClassUri ||
       uri == kTimetableUri || uri == kDayScansUri ||
       uri == kDaySummaryUri || uri == kDayAdjustmentsUri ||
       uri == kHistoricalGridUri);
}
bool Esp32AdminRosterHttpHandler::canRaw(WebServer& server, const String& uri) {
  return server.method() == HTTP_POST &&
      ((uri == kRosterUri && writer_ != nullptr) ||
       ((uri == kTodayStatusUri || uri == kTodayNoClassUri ||
         uri == kTodayNoClassReviewUri ||
         uri == "/api/v1/mark" || uri == "/api/v1/no-class" ||
         uri == kHistoricalMarkUri) &&
        attendanceWriter_ != nullptr));
}
bool Esp32AdminRosterHttpHandler::canUpload(WebServer& server, const String& uri) {
  return server.method() == HTTP_POST &&
      ((uri == kRosterUri && writer_ != nullptr) ||
       ((uri == kTodayStatusUri || uri == kTodayNoClassUri ||
         uri == kTodayNoClassReviewUri ||
         uri == "/api/v1/mark" || uri == "/api/v1/no-class" ||
         uri == kHistoricalMarkUri) &&
        attendanceWriter_ != nullptr));
}
void Esp32AdminRosterHttpHandler::receiveMutationBody_(
    WebServer& server, HTTPRaw& rawData) {
  if (rawData.status == RAW_START) {
    const String contentType = server.header("Content-Type");
    const String contentLength = server.header("Content-Length");
    const String transferEncoding = server.header("Transfer-Encoding");
    (void)mutationBody_.begin(
        contentType.c_str(), contentLength.c_str(),
        server.clientContentLength(), transferEncoding.length() != 0);
  } else if (rawData.status == RAW_WRITE) {
    (void)mutationBody_.append(rawData.totalSize, rawData.currentSize,
                               rawData.buf);
  } else if (rawData.status == RAW_END) {
    (void)mutationBody_.finish();
  } else if (rawData.status == RAW_ABORTED) {
    mutationBody_.abort();
  }
}
void Esp32AdminRosterHttpHandler::raw(
    WebServer& server, const String& uri, HTTPRaw& rawData) {
  if (server.method() == HTTP_POST &&
      ((uri == kRosterUri && writer_ != nullptr) ||
       ((uri == kTodayStatusUri || uri == kTodayNoClassUri ||
         uri == kTodayNoClassReviewUri ||
         uri == "/api/v1/mark" || uri == "/api/v1/no-class" ||
         uri == kHistoricalMarkUri) &&
        attendanceWriter_ != nullptr)))
    receiveMutationBody_(server, rawData);
}
void Esp32AdminRosterHttpHandler::upload(
    WebServer& server, const String& uri, HTTPUpload& uploadData) {
  (void)server;
  if (uploadData.status == UPLOAD_FILE_START &&
      ((uri == kRosterUri && writer_ != nullptr) ||
       ((uri == kTodayStatusUri || uri == kTodayNoClassUri ||
         uri == kTodayNoClassReviewUri ||
         uri == "/api/v1/mark" || uri == "/api/v1/no-class" ||
         uri == kHistoricalMarkUri) &&
        attendanceWriter_ != nullptr)))
    mutationBody_.reject(415, "multipart_upload_not_supported");
}
bool Esp32AdminRosterHttpHandler::parseAfter_(
    WebServer& server, uint16_t& after) const {
  after = 0;
  if (server.args() == 0) return true;
  if (server.args() != 1 || server.argName(0) != "after") return false;
  String value = server.arg("after");
  if (value.length() == 0 || value.length() > 3) { value = ""; return false; }
  uint16_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') { value = ""; return false; }
    parsed = static_cast<uint16_t>(parsed * 10u +
        static_cast<uint16_t>(c - '0'));
    if (parsed > MAX_ID) { value = ""; return false; }
  }
  value = "";
  after = parsed;
  return true;
}
bool Esp32AdminRosterHttpHandler::parsePendingNoClassQuery_(
    WebServer& server, size_t& after) const {
  after = 0;
  if (server.args() == 0) return true;
  if (server.args() != 1 || server.argName(0) != "after") return false;
  String value = server.arg("after");
  if (value.length() == 0 || value.length() > 3) { value = ""; return false; }
  size_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') { value = ""; return false; }
    parsed = parsed * 10u + static_cast<size_t>(c - '0');
    if (parsed > MAX_CLOSES) { value = ""; return false; }
  }
  value = "";
  after = parsed;
  return true;
}
bool Esp32AdminRosterHttpHandler::parseTodayQuery_(
    WebServer& server, uint8_t& session, uint16_t& after) const {
  if (server.args() != 2) return false;
  bool hasSession = false, hasAfter = false;
  uint32_t parsedSession = 0, parsedAfter = 0;
  for (int i = 0; i < server.args(); ++i) {
    const String name = server.argName(i);
    const String value = server.arg(i);
    uint32_t parsed = 0;
    if (name == "session" && !hasSession) {
      // 2 is the server-resolved default: active session, otherwise the
      // most recently ended configured session, otherwise first enabled.
      if (!parseDecimal(value, 2, 1, parsed)) return false;
      parsedSession = parsed;
      hasSession = true;
    } else if (name == "after" && !hasAfter) {
      if (!parseDecimal(value, MAX_ID, 3, parsed)) return false;
      parsedAfter = parsed;
      hasAfter = true;
    } else {
      return false;
    }
  }
  if (!hasSession || !hasAfter) return false;
  session = static_cast<uint8_t>(parsedSession);
  after = static_cast<uint16_t>(parsedAfter);
  return true;
}

bool Esp32AdminRosterHttpHandler::parseDayScansQuery_(
    WebServer& server, int32_t& day, uint8_t& session,
    uint32_t& cursor) const {
  if (server.args() != 3) return false;
  bool hasDay = false, hasSession = false, hasCursor = false;
  uint32_t parsedDay = 0, parsedSession = 0, parsedCursor = 0;
  for (int i = 0; i < server.args(); ++i) {
    const String name = server.argName(i);
    const String value = server.arg(i);
    uint32_t parsed = 0;
    if (name == "day" && !hasDay) {
      if (!parseDecimal(value, MAX_SUPPORTED_DAY, 5, parsed)) return false;
      parsedDay = parsed;
      hasDay = true;
    } else if (name == "session" && !hasSession) {
      if (!parseDecimal(value, 1, 1, parsed)) return false;
      parsedSession = parsed;
      hasSession = true;
    } else if (name == "cursor" && !hasCursor) {
      if (!parseDecimal(value, UINT32_MAX, 10, parsed)) return false;
      parsedCursor = parsed;
      hasCursor = true;
    } else {
      return false;
    }
  }
  if (!hasDay || !hasSession || !hasCursor) return false;
  day = static_cast<int32_t>(parsedDay);
  session = static_cast<uint8_t>(parsedSession);
  cursor = parsedCursor;
  return true;
}

bool Esp32AdminRosterHttpHandler::parseDaySummaryQuery_(
    WebServer& server, int32_t& day, uint8_t& session, uint16_t& after) const {
  if (server.args() != 3) return false;
  bool hasDay = false, hasSession = false, hasAfter = false;
  uint32_t parsedDay = 0, parsedSession = 0, parsedAfter = 0;
  for (int i = 0; i < server.args(); ++i) {
    const String name = server.argName(i);
    const String value = server.arg(i);
    uint32_t parsed = 0;
    if (name == "day" && !hasDay) {
      if (!parseDecimal(value, MAX_SUPPORTED_DAY, 5, parsed)) return false;
      parsedDay = parsed;
      hasDay = true;
    } else if (name == "session" && !hasSession) {
      if (!parseDecimal(value, 1, 1, parsed)) return false;
      parsedSession = parsed;
      hasSession = true;
    } else if (name == "after" && !hasAfter) {
      if (!parseDecimal(value, MAX_ID, 3, parsed)) return false;
      parsedAfter = parsed;
      hasAfter = true;
    } else {
      return false;
    }
  }
  if (!hasDay || !hasSession || !hasAfter) return false;
  day = static_cast<int32_t>(parsedDay);
  session = static_cast<uint8_t>(parsedSession);
  after = static_cast<uint16_t>(parsedAfter);
  return true;
}

bool Esp32AdminRosterHttpHandler::parseHistoricalGridQuery_(
    WebServer& server, int32_t& fromDay, int32_t& toDay,
    uint16_t& after) const {
  if (server.args() != 3) return false;
  bool hasFrom = false, hasTo = false, hasAfter = false;
  uint32_t parsedFrom = 0, parsedTo = 0, parsedAfter = 0;
  for (int i = 0; i < server.args(); ++i) {
    const String name = server.argName(i);
    const String value = server.arg(i);
    uint32_t parsed = 0;
    if (name == "from" && !hasFrom) {
      if (!parseDecimal(value, MAX_SUPPORTED_DAY, 5, parsed)) return false;
      parsedFrom = parsed;
      hasFrom = true;
    } else if (name == "to" && !hasTo) {
      if (!parseDecimal(value, MAX_SUPPORTED_DAY, 5, parsed)) return false;
      parsedTo = parsed;
      hasTo = true;
    } else if (name == "after" && !hasAfter) {
      if (!parseDecimal(value, MAX_ID, 3, parsed)) return false;
      parsedAfter = parsed;
      hasAfter = true;
    } else {
      return false;
    }
  }
  if (!hasFrom || !hasTo || !hasAfter) return false;
  fromDay = static_cast<int32_t>(parsedFrom);
  toDay = static_cast<int32_t>(parsedTo);
  after = static_cast<uint16_t>(parsedAfter);
  return true;
}

bool Esp32AdminRosterHttpHandler::appendByte_(char value) {
  if (responseLength_ + 1 >= sizeof response_) return false;
  response_[responseLength_++] = value;
  response_[responseLength_] = '\0';
  return true;
}
bool Esp32AdminRosterHttpHandler::appendLiteral_(const char* value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length >= sizeof response_ - responseLength_) return false;
  memcpy(response_ + responseLength_, value, length);
  responseLength_ += length;
  response_[responseLength_] = '\0';
  return true;
}
bool Esp32AdminRosterHttpHandler::appendFormat_(const char* format, ...) {
  if (format == nullptr || responseLength_ >= sizeof response_) return false;
  const size_t available = sizeof response_ - responseLength_;
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(response_ + responseLength_, available, format, args);
  va_end(args);
  if (length < 0 || static_cast<size_t>(length) >= available) return false;
  responseLength_ += static_cast<size_t>(length);
  return true;
}
bool Esp32AdminRosterHttpHandler::appendJsonString_(
    const char* value, size_t length) {
  if (value == nullptr || !appendByte_('"')) return false;
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c == '"' || c == '\\') {
      if (!appendByte_('\\') || !appendByte_(static_cast<char>(c))) return false;
    } else if (c < 0x20) {
      char escaped[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0f], '\0'};
      if (!appendLiteral_(escaped)) return false;
    } else if (!appendByte_(static_cast<char>(c))) return false;
  }
  return appendByte_('"');
}
bool Esp32AdminRosterHttpHandler::buildPage_(uint16_t after) {
  responseLength_ = 0;
  response_[0] = '\0';
  if (!roster_.historyComplete() || !roster_.healthy()) return false;
  if (!appendFormat_("{\"count\":%lu,\"active_count\":%lu,\"items\":[",
      static_cast<unsigned long>(roster_.knownCount()),
      static_cast<unsigned long>(roster_.activeCount()))) return false;
  uint8_t emitted = 0;
  uint16_t lastId = 0;
  bool hasMore = false;
  for (uint16_t id = static_cast<uint16_t>(after + 1); id <= MAX_ID; ++id) {
    if (!roster_.known(id)) continue;
    if (emitted == kRosterPageSize) { hasMore = true; break; }
    char name[ROSTER_NAME_MAX + 1] = {};
    const size_t nameLength = roster_.nameOf(id, name);
    if (nameLength == 0 || nameLength > ROSTER_NAME_MAX) {
      wipe(name, sizeof name); return false;
    }
    const bool prefixOk = emitted == 0 || appendByte_(',');
    const bool itemOk = prefixOk &&
        appendFormat_("{\"id\":%u,\"name\":", static_cast<unsigned>(id)) &&
        appendJsonString_(name, nameLength) &&
        appendFormat_(",\"active\":%s}", roster_.active(id) ? "true" : "false");
    wipe(name, sizeof name);
    if (!itemOk) return false;
    lastId = id;
    ++emitted;
  }
  if (hasMore) {
    if (!appendFormat_("],\"next_after\":%u}", static_cast<unsigned>(lastId))) return false;
  } else if (!appendLiteral_("],\"next_after\":null}")) return false;
  return roster_.healthy();
}
bool Esp32AdminRosterHttpHandler::buildTimetable_() {
  responseLength_ = 0;
  response_[0] = '\0';
  // ConfigBootCoordinator exposes only a validated, long-lived READY config.
  // Revalidate it and serialize only non-secret fields.
  if (!validateConfig(config_)) return false;
  if (!appendFormat_(
      "{\"utc_offset_min\":%d,\"late_min\":%u,\"scan_lead_min\":%u,"
      "\"go_live_day\":%ld,\"days\":[",
      static_cast<int>(config_.params.utcOffsetMin),
      static_cast<unsigned>(config_.params.lateMin),
      static_cast<unsigned>(config_.params.scanLeadMin),
      static_cast<long>(config_.goLiveDay))) return false;

  static const char* const names[] = {
      "Monday", "Tuesday", "Wednesday", "Thursday", "Friday"};
  for (uint8_t day = 0; day < 5; ++day) {
    if (day != 0 && !appendByte_(',')) return false;
    if (!appendFormat_("{\"weekday\":%u,\"name\":\"%s\",\"am\":",
                       static_cast<unsigned>(day), names[day])) return false;

    const Slot& am = config_.tt.day[day][0];
    if (am.enabled) {
      if (!appendFormat_("{\"enabled\":true,\"start_min\":%u,\"end_min\":%u}",
                         static_cast<unsigned>(am.start),
                         static_cast<unsigned>(am.end))) return false;
    } else if (!appendLiteral_("{\"enabled\":false,\"start_min\":null,\"end_min\":null}")) {
      return false;
    }

    if (!appendLiteral_(",\"pm\":")) return false;
    const Slot& pm = config_.tt.day[day][1];
    if (pm.enabled) {
      if (!appendFormat_("{\"enabled\":true,\"start_min\":%u,\"end_min\":%u}",
                         static_cast<unsigned>(pm.start),
                         static_cast<unsigned>(pm.end))) return false;
    } else if (!appendLiteral_("{\"enabled\":false,\"start_min\":null,\"end_min\":null}")) {
      return false;
    }
    if (!appendByte_('}')) return false;
  }
  return appendLiteral_("]}");
}

bool Esp32AdminRosterHttpHandler::buildPendingAutomaticNoClass_(size_t after) {
  responseLength_ = 0;
  response_[0] = '\0';
  if (logIndex_ == nullptr || log_.faulted() ||
      !log_.usesStorage(monthData_) ||
      !logIndex_->pendingAutoNoClassHealthy() ||
      logIndex_->pendingAutoNoClassCount() > MAX_AUTO_NOCLASS_PENDING ||
      after > logIndex_->pendingAutoNoClassCount() ||
      log_.count() > UINT32_MAX) return false;

  const size_t pendingCount = logIndex_->pendingAutoNoClassCount();
  const size_t recordCount = log_.count();
  const uint64_t generation = log_.openGeneration();
  if (generation == 0) return false;
  const uint32_t generationHigh = static_cast<uint32_t>(generation >> 32);
  const uint32_t generationLow = static_cast<uint32_t>(generation);
  if (!appendFormat_(
      "{\"ready\":true,\"log_records\":%lu,"
      "\"log_generation_high\":%lu,\"log_generation_low\":%lu,"
      "\"pending_count\":%lu,\"items\":[",
      static_cast<unsigned long>(recordCount),
      static_cast<unsigned long>(generationHigh),
      static_cast<unsigned long>(generationLow),
      static_cast<unsigned long>(pendingCount))) return false;

  size_t cursor = after;
  uint8_t emitted = 0;
  while (cursor < pendingCount && emitted < kPendingAutoNoClassPageSize) {
    PendingAutomaticNoClass item = {};
    if (!logIndex_->pendingAutoNoClassAt(cursor, item)) return false;
    ++cursor;
    if (item.day > MAX_SUPPORTED_DAY || item.session > 1 ||
        item.declarationSequence == 0 || item.expectedCount == 0 ||
        item.reserved != 0 || item.declarationSequence > recordCount)
      return false;
    Event declaration = {};
    if (!log_.at(static_cast<size_t>(item.declarationSequence - 1u), declaration) ||
        declaration.seq != item.declarationSequence ||
        declaration.type != EV_AUTO_NOCLASS_SET || declaration.epoch < EPOCH_2020 ||
        declaration.b != item.day || flagSession(declaration.flags) != item.session ||
        declaration.a != item.expectedCount)
      return false;
    if (emitted != 0 && !appendLiteral_(",")) return false;
    if (!appendFormat_(
        "{\"day\":%ld,\"session\":%u,\"auto_sequence\":%lu,"
        "\"expected_count\":%u,\"detected_epoch\":%lu}",
        static_cast<long>(item.day), static_cast<unsigned>(item.session),
        static_cast<unsigned long>(item.declarationSequence),
        static_cast<unsigned>(item.expectedCount),
        static_cast<unsigned long>(declaration.epoch))) return false;
    ++emitted;
  }
  if (log_.count() != recordCount || log_.openGeneration() != generation ||
      !logIndex_->evidenceHealthy() ||
      logIndex_->pendingAutoNoClassCount() != pendingCount) return false;
  return cursor < pendingCount
      ? appendFormat_("],\"next_after\":%lu}",
                      static_cast<unsigned long>(cursor))
      : appendLiteral_("],\"next_after\":null}");
}

bool Esp32AdminRosterHttpHandler::buildToday_(uint8_t session, uint16_t after) {
  responseLength_ = 0;
  response_[0] = '\0';
  if (session > 2 || after > MAX_ID || !validateConfig(config_) ||
      rtcClock_ == nullptr || clockHealth_ == nullptr ||
      !clockHealth_->clockTrusted() || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy()) return false;

  Ds3231Reading reading = {0, false};
  if (rtcClock_->readUtcEpoch(0, reading) != DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch()))
    return false;
  const int32_t day = dayNumber(reading.epochUtc, config_.params.utcOffsetMin);
  if (!apiDaySupported(day) || day < config_.goLiveDay || weekdayIndex(day) > 4)
    return false;
  const uint16_t minute = minuteOfDay(reading.epochUtc, config_.params.utcOffsetMin);
  const int currentSession = sessionAt(config_.tt, config_.params, reading.epochUtc);
  if (session == 2) {
    const Slot& am = config_.tt.day[weekdayIndex(day)][0];
    const Slot& pm = config_.tt.day[weekdayIndex(day)][1];
    if (currentSession >= 0 && currentSession <= 1 &&
        config_.tt.day[weekdayIndex(day)][currentSession].enabled) {
      session = static_cast<uint8_t>(currentSession);
    } else if (pm.enabled && hasEnded(pm, minute)) {
      session = 1;
    } else if (am.enabled && hasEnded(am, minute)) {
      session = 0;
    } else if (am.enabled) {
      session = 0;
    } else if (pm.enabled) {
      session = 1;
    } else {
      return false;
    }
  }
  if (session > 1 || !config_.tt.day[weekdayIndex(day)][session].enabled)
    return false;

  bool viewReady = false;
  if (!prepareDayView_(day, session, viewReady)) return false;
  const size_t recordCount = dayCacheRecordCount_;
  const uint64_t generation = dayCacheGeneration_;
  const uint32_t generationHigh = static_cast<uint32_t>(generation >> 32);
  const uint32_t generationLow = static_cast<uint32_t>(generation);
  const Slot& slot = config_.tt.day[weekdayIndex(day)][session];
  const bool started = hasStarted(slot, minute);
  const bool ended = hasEnded(slot, minute);
  const bool noClass = dayView_.noClassDay || dayView_.noClassSession;
  const bool writeGate = storageWritesAuthorized_ != nullptr &&
      *storageWritesAuthorized_ && !applicationFaulted_ &&
      attendanceWriter_ != nullptr && !attendanceWriter_->faulted() &&
      !log_.faulted() && clockHealth_->clockTrusted() &&
      (health_.state() == H_OK || health_.state() == H_SENSOR_FAULT);
  const bool editable = viewReady && !dayView_.closed && !noClass &&
      currentSession == session && started && !ended && writeGate;
  // Class cancellation is a session-level administrative action and remains
  // available before or after the scan window, including after closure.
  const bool noClassEditable = viewReady && !dayView_.noClassDay && writeGate;
  const char* noClassScope = dayView_.noClassDay ? "day" :
      dayView_.noClassSession ? "session" : "none";
  const char* sessionState = noClass ? "no_class" :
      !started ? "upcoming" : dayView_.closed ?
        (dayView_.coverage ? "closed_complete" : "closed_unverified") :
      ended ? "closing" : "open";

  if (!viewReady) {
    return appendFormat_(
        "{\"ready\":false,\"day\":%ld,\"session\":%u,"
        "\"processed\":%lu,\"total\":%lu,"
        "\"log_generation_high\":%lu,\"log_generation_low\":%lu,"
        "\"log_records\":%lu,\"items\":[],\"next_after\":null}",
        static_cast<long>(day), static_cast<unsigned>(session),
        static_cast<unsigned long>(dayCacheNextRecord_),
        static_cast<unsigned long>(recordCount),
        static_cast<unsigned long>(generationHigh),
        static_cast<unsigned long>(generationLow),
        static_cast<unsigned long>(recordCount));
  }

  uint16_t total = 0;
  uint16_t present = 0, late = 0, pending = 0, excused = 0;
  uint16_t absent = 0, unverified = 0, noClassCount = 0, upcoming = 0;
  for (uint16_t id = 1; id <= MAX_ID; ++id) {
    const bool expected = dayView_.closed
        ? bitmapTest(dayView_.expected, id) : roster_.expected(id, day);
    if (!expected) continue;
    const Derived status = statusFor(dayView_, id, !dayView_.closed, started);
    ++total;
    switch (status) {
      case D_PRESENT: ++present; break;
      case D_LATE: ++late; break;
      case D_PENDING: ++pending; break;
      case D_EXCUSED: ++excused; break;
      case D_ABSENT: ++absent; break;
      case D_UNVERIFIED: ++unverified; break;
      case D_NOCLASS: ++noClassCount; break;
      case D_UPCOMING: ++upcoming; break;
      default: return false;
    }
  }

  // A single validated DayView + roster replay supplies both the rows and the
  // counters. Pagination identity is the exact log generation/count pair.
  const int64_t localEpoch = static_cast<int64_t>(reading.epochUtc) +
      static_cast<int64_t>(config_.params.utcOffsetMin) * 60;
  if (localEpoch < 0 || localEpoch > UINT32_MAX) return false;
  const time_t localTime = static_cast<time_t>(localEpoch);
  struct tm localParts = {};
  if (gmtime_r(&localTime, &localParts) == nullptr) return false;
  const char* sessionName = session == 0 ? "AM" : "PM";
  if (!appendFormat_(
      "{\"ready\":true,\"day\":%ld,\"local_date\":\"%04d-%02d-%02d\","
      "\"session\":%u,\"session_name\":\"%s\",\"current_session\":%d,"
      "\"default_session\":%u,\"session_state\":\"%s\","
      "\"server_epoch\":%lu,\"local_minute\":%u,\"closed\":%s,"
      "\"coverage_complete\":%s,\"no_class\":%s,"
      "\"no_class_scope\":\"%s\",\"no_class_session\":%s,"
      "\"auto_no_class_pending\":%s,\"auto_no_class_confirmed\":%s,"
      "\"auto_no_class_sequence\":%lu,\"auto_no_class_expected\":%u,"
      "\"no_class_editable\":%s,\"editable\":%s,\"expected_count\":%u,"
      "\"log_generation_high\":%lu,\"log_generation_low\":%lu,"
      "\"log_records\":%lu,\"summary\":{"
      "\"present\":%u,\"late\":%u,\"pending\":%u,\"excused\":%u,"
      "\"absent\":%u,\"unverified\":%u,\"no_class\":%u,\"upcoming\":%u},"
      "\"items\":[",
      static_cast<long>(day), localParts.tm_year + 1900,
      localParts.tm_mon + 1, localParts.tm_mday,
      static_cast<unsigned>(session), sessionName, currentSession,
      static_cast<unsigned>(session), sessionState,
      static_cast<unsigned long>(reading.epochUtc), static_cast<unsigned>(minute),
      dayView_.closed ? "true" : "false",
      dayView_.closed && dayView_.coverage ? "true" : "false",
      noClass ? "true" : "false", noClassScope,
      dayView_.noClassSession ? "true" : "false",
      dayView_.autoNoClassPending ? "true" : "false",
      dayView_.autoNoClassConfirmed ? "true" : "false",
      static_cast<unsigned long>(dayView_.autoNoClassSequence),
      static_cast<unsigned>(dayView_.autoNoClassExpectedCount),
      noClassEditable && !dayView_.autoNoClassPending &&
          !dayView_.autoNoClassConfirmed ? "true" : "false",
      editable ? "true" : "false", static_cast<unsigned>(total),
      static_cast<unsigned long>(generationHigh),
      static_cast<unsigned long>(generationLow),
      static_cast<unsigned long>(recordCount),
      static_cast<unsigned>(present), static_cast<unsigned>(late),
      static_cast<unsigned>(pending), static_cast<unsigned>(excused),
      static_cast<unsigned>(absent), static_cast<unsigned>(unverified),
      static_cast<unsigned>(noClassCount), static_cast<unsigned>(upcoming)))
    return false;

  uint8_t emitted = 0;
  uint16_t lastId = after;
  bool hasMore = false;
  for (uint16_t id = static_cast<uint16_t>(after + 1u); id <= MAX_ID; ++id) {
    const bool expected = dayView_.closed
        ? bitmapTest(dayView_.expected, id) : roster_.expected(id, day);
    if (!expected) continue;
    if (emitted >= kTodayPageSize) {
      hasMore = true;
      break;
    }
    const Derived status = statusFor(dayView_, id, !dayView_.closed, started);
    const char* label = nullptr;
    const char* source = "derived";
    switch (status) {
      case D_PRESENT: label = "present"; break;
      case D_LATE: label = "late"; break;
      case D_PENDING: label = "pending"; break;
      case D_EXCUSED: label = "excused"; break;
      case D_ABSENT: label = "absent"; break;
      case D_UNVERIFIED: label = "unverified"; break;
      case D_NOCLASS: label = "no_class"; break;
      case D_UPCOMING: label = "upcoming"; break;
      default: return false;
    }
    if (!dayView_.noClassDay && !dayView_.noClassSession &&
        dayView_.mark[id] != ST_CLEAR) source = "manual";
    else if (!dayView_.noClassDay && !dayView_.noClassSession &&
             dayView_.scan[id] != ST_CLEAR) source = "fingerprint";
    char name[ROSTER_NAME_MAX + 1] = {};
    const size_t nameLength = roster_.nameOf(id, name);
    bool itemOk = nameLength != 0 && nameLength <= ROSTER_NAME_MAX &&
        (emitted == 0 || appendByte_(',')) &&
        appendFormat_("{\"student_id\":%u,\"name\":",
                      static_cast<unsigned>(id)) &&
        appendJsonString_(name, nameLength) &&
        appendFormat_(",\"status\":\"%s\",\"source\":\"%s\",\"scan_epoch\":",
                      label, source);
    if (itemOk) {
      itemOk = todayScanEpoch_[id] == 0
          ? appendLiteral_("null")
          : appendFormat_("%lu", static_cast<unsigned long>(todayScanEpoch_[id]));
    }
    uint8_t correctionSeconds = 0;
    if (editable && todayLastStatusSequence_[id] != 0 &&
        todayLastStatusSequence_[id] == todayRevision_[id] &&
        (dayView_.mark[id] == ST_PRESENT || dayView_.mark[id] == ST_LATE ||
         dayView_.mark[id] == ST_EXCUSED)) {
      correctionSeconds = correctionSecondsRemaining(
          reading.epochUtc, todayLastStatusEpoch_[id]);
    }
    if (itemOk)
      itemOk = appendFormat_(",\"revision\":%lu,\"correction_seconds\":%u}",
                            static_cast<unsigned long>(todayRevision_[id]),
                            static_cast<unsigned>(correctionSeconds));
    wipe(name, sizeof name);
    if (!itemOk) return false;
    lastId = id;
    ++emitted;
  }
  if (hasMore) {
    if (!appendFormat_("],\"next_after\":%u}", static_cast<unsigned>(lastId))) return false;
  } else if (!appendLiteral_("],\"next_after\":null}")) return false;
  return !log_.faulted() && log_.count() == recordCount &&
      log_.openGeneration() == generation && roster_.historyComplete() &&
      roster_.healthy();
}

bool Esp32AdminRosterHttpHandler::buildDayScans_(
    int32_t day, uint8_t session, uint32_t cursor) {
  responseLength_ = 0;
  response_[0] = '\0';
  const size_t total = log_.count();
  const size_t maxSize = static_cast<size_t>(-1);
  if (!validateConfig(config_) || log_.faulted() ||
      !log_.usesStorage(monthData_) ||
      !roster_.historyComplete() || !roster_.healthy() ||
      !apiDaySupported(day) ||
      session > 1 || weekdayIndex(day) > 4 ||
      !config_.tt.day[weekdayIndex(day)][session].enabled ||
      total > static_cast<size_t>(UINT32_MAX) ||
      total > maxSize / REC_SIZE || cursor > total) return false;

  if (!appendFormat_("{\"day\":%ld,\"session\":%u,\"items\":[",
      static_cast<long>(day), static_cast<unsigned>(session))) return false;

  // Use the exact MonthSet instance backing LogStore, but read contiguous
  // record batches. Repeated LogStore::at() calls reopen/query storage per
  // record and would block the serialized application loop unnecessarily.
  uint8_t raw[kDayScanReadBatchRecords * REC_SIZE] = {};
  size_t position = cursor;
  uint16_t scanned = 0;
  uint8_t emitted = 0;
  bool batchOk = true;
  while (position < total &&
         scanned < kDayScanRecordBudget &&
         emitted < kDayScanPageSize) {
    size_t batchCount = total - position;
    const size_t batchLimit = kDayScanRecordBudget - scanned;
    if (batchCount > kDayScanReadBatchRecords)
      batchCount = kDayScanReadBatchRecords;
    if (batchCount > batchLimit) batchCount = batchLimit;
    if (batchCount == 0 || position > maxSize / REC_SIZE ||
        batchCount > maxSize / REC_SIZE) {
      batchOk = false;
      break;
    }

    const size_t byteOffset = position * REC_SIZE;
    const size_t byteCount = batchCount * REC_SIZE;
    if (!monthData_.read(byteOffset, raw, byteCount)) {
      batchOk = false;
      break;
    }
    for (size_t i = 0; i < batchCount && emitted < kDayScanPageSize; ++i) {
      Event event = {};
      if (!decode(raw + i * REC_SIZE, event) ||
          event.seq != static_cast<uint32_t>(position + 1u)) {
        batchOk = false;
        break;
      }
      ++position;
      ++scanned;
      if (event.type != EV_SCAN) continue;

      const uint8_t status = flagStatus(event.flags);
      if (event.a < 1 || event.a > MAX_ID || event.epoch == 0 ||
          (status != ST_PRESENT && status != ST_LATE)) {
        batchOk = false;
        break;
      }
      if (flagSession(event.flags) != session ||
          dayNumber(event.epoch, config_.params.utcOffsetMin) != day) continue;

      const char* statusName = status == ST_PRESENT ? "present" : "late";
      if ((emitted != 0 && !appendByte_(',')) ||
          !appendFormat_("{\"student_id\":%u,\"status\":\"%s\",\"utc_epoch\":%lu}",
            static_cast<unsigned>(event.a), statusName,
            static_cast<unsigned long>(event.epoch))) {
        batchOk = false;
        break;
      }
      ++emitted;
    }
    wipe(raw, sizeof raw);
    if (!batchOk) break;
  }
  wipe(raw, sizeof raw);
  if (!batchOk) return false;

  if (position < total) {
    if (!appendFormat_("],\"next_cursor\":%lu}",
                       static_cast<unsigned long>(position))) return false;
  } else if (!appendLiteral_("],\"next_cursor\":null}")) {
    return false;
  }
  return !log_.faulted() && log_.usesStorage(monthData_) &&
      roster_.historyComplete() && roster_.healthy();
}

bool Esp32AdminRosterHttpHandler::buildDayAdjustments_(
    int32_t day, uint8_t session, uint32_t cursor) {
  responseLength_ = 0;
  response_[0] = '\0';
  const size_t total = log_.count();
  const size_t maxSize = static_cast<size_t>(-1);
  if (!validateConfig(config_) || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy() ||
      !apiDaySupported(day) ||
      weekdayIndex(day) > 4 || session > 1 ||
      total > static_cast<size_t>(UINT32_MAX) ||
      total > maxSize / REC_SIZE || cursor > total) return false;

  if (!appendFormat_("{\"day\":%ld,\"session\":%u,\"items\":[",
      static_cast<long>(day), static_cast<unsigned>(session))) return false;

  uint8_t raw[kDayAdjustmentReadBatchRecords * REC_SIZE] = {};
  size_t position = cursor;
  uint16_t scanned = 0;
  uint8_t emitted = 0;
  bool batchOk = true;
  while (position < total && scanned < kDayAdjustmentRecordBudget &&
         emitted < kDayAdjustmentPageSize) {
    size_t batchCount = total - position;
    const size_t batchLimit = kDayAdjustmentRecordBudget - scanned;
    if (batchCount > kDayAdjustmentReadBatchRecords)
      batchCount = kDayAdjustmentReadBatchRecords;
    if (batchCount > batchLimit) batchCount = batchLimit;
    if (batchCount == 0 || position > maxSize / REC_SIZE ||
        batchCount > maxSize / REC_SIZE) {
      batchOk = false;
      break;
    }
    if (!monthData_.read(position * REC_SIZE, raw, batchCount * REC_SIZE)) {
      batchOk = false;
      break;
    }
    for (size_t i = 0; i < batchCount && emitted < kDayAdjustmentPageSize; ++i) {
      Event event = {};
      if (!decode(raw + i * REC_SIZE, event) ||
          event.seq != static_cast<uint32_t>(position + 1u)) {
        batchOk = false;
        break;
      }
      const size_t eventIndex = position;
      ++position;
      ++scanned;
      const bool mark = event.type == EV_MARK && event.b == day &&
          flagSession(event.flags) == session;
      const bool todayStatus = event.type == EV_TODAY_STATUS &&
          event.b == day && flagSession(event.flags) == session;
      const bool noClass =
          (event.type == EV_NOCLASS_SET || event.type == EV_NOCLASS_CLEAR) &&
          event.b == day &&
          (flagWholeDay(event.flags) || flagSession(event.flags) == session);
      const bool automaticNoClassSet = event.type == EV_AUTO_NOCLASS_SET &&
          event.b == day && flagSession(event.flags) == session;
      const bool automaticNoClassReview =
          event.type == EV_AUTO_NOCLASS_REVIEW && event.b == day &&
          flagSession(event.flags) == session;
      if (!mark && !todayStatus && !noClass && !automaticNoClassSet &&
          !automaticNoClassReview) continue;
      if ((automaticNoClassSet || automaticNoClassReview)
              ? event.epoch < EPOCH_2020 : event.epoch == 0) {
        batchOk = false;
        break;
      }

      // Recover audit metadata from the complete commit group. The helper is
      // platform-neutral and host-tested so field ordering, JSON quoting and
      // legacy compatibility are not hidden inside this ESP32 adapter.
      Event previous = {}, marker = {}, audit = {};
      Event todayCas0 = {}, todayCas1 = {}, todayCas2 = {};
      const Event* markerPointer = nullptr;
      const Event* auditPointer = nullptr;
      const Event* todayCas0Pointer = nullptr;
      const Event* todayCas1Pointer = nullptr;
      const Event* todayCas2Pointer = nullptr;
      if (eventIndex >= 1) {
        if (!log_.at(eventIndex - 1, previous)) {
          batchOk = false;
          break;
        }
        if (previous.type == EV_TODAY_CAS) {
          if (todayStatus) {
            if (eventIndex < 5 ||
                !log_.at(eventIndex - 5, marker) ||
                !log_.at(eventIndex - 4, audit) ||
                !log_.at(eventIndex - 3, todayCas0) ||
                !log_.at(eventIndex - 2, todayCas1) ||
                !log_.at(eventIndex - 1, todayCas2)) {
              batchOk = false;
              break;
            }
            todayCas0Pointer = &todayCas0;
            todayCas1Pointer = &todayCas1;
            todayCas2Pointer = &todayCas2;
          } else if (automaticNoClassReview) {
            if (eventIndex < 5 ||
                !log_.at(eventIndex - 5, marker) ||
                !log_.at(eventIndex - 4, audit) ||
                !log_.at(eventIndex - 3, todayCas0) ||
                !log_.at(eventIndex - 2, todayCas1) ||
                !log_.at(eventIndex - 1, todayCas2)) {
              batchOk = false;
              break;
            }
            todayCas0Pointer = &todayCas0;
            todayCas1Pointer = &todayCas1;
            todayCas2Pointer = &todayCas2;
          } else {
            batchOk = false;
            break;
          }
          markerPointer = &marker;
          auditPointer = &audit;
        } else if (previous.type == EV_ADMIN_AUDIT) {
          if (eventIndex < 2 || !log_.at(eventIndex - 2, marker)) {
            batchOk = false;
            break;
          }
          markerPointer = &marker;
          audit = previous;
          auditPointer = &audit;
        } else if (previous.type == EV_ADMIN_MUTATION) {
          marker = previous;
          markerPointer = &marker;
        }
      }
      AdminMutationAuditJsonFields auditFields = {};
      if (!automaticNoClassSet &&
          !formatAdminMutationAuditJsonFields(markerPointer, auditPointer,
                                               todayCas0Pointer, todayCas1Pointer,
                                               todayCas2Pointer, event,
                                               auditFields)) {
        batchOk = false;
        break;
      }
      if (emitted != 0 && !appendByte_(',')) { batchOk = false; break; }
      if (mark || todayStatus) {
        const uint8_t status = flagStatus(event.flags);
        if (event.a < 1 || event.a > MAX_ID ||
            (todayStatus
                ? (status != ST_PRESENT && status != ST_LATE &&
                   status != ST_PENDING && status != ST_EXCUSED)
                : status > ST_EXCUSED)) {
          batchOk = false;
          break;
        }
        static const char* const statuses[] = {
            "clear", "present", "late", "excused", "pending"};
        if (!appendFormat_(
            "{\"kind\":\"%s\",\"student_id\":%u,\"status\":\"%s\","
            "\"utc_epoch\":%lu,\"sequence\":%lu,"
            "\"request_id\":%s,\"reason\":%s,"
            "\"admin_session\":%s}",
            todayStatus ? "today_status" : "mark",
            static_cast<unsigned>(event.a), statuses[status],
            static_cast<unsigned long>(event.epoch),
            static_cast<unsigned long>(event.seq), auditFields.requestId,
            auditFields.reason, auditFields.adminSession)) {
          batchOk = false;
          break;
        }
      } else if (automaticNoClassSet) {
        if (event.a < 1 || event.a > MAX_ID ||
            !appendFormat_(
                "{\"kind\":\"no_class\",\"scope\":\"session\","
                "\"operation\":\"set\",\"source\":\"automatic\","
                "\"review\":\"pending\",\"session\":%u,"
                "\"utc_epoch\":%lu,\"sequence\":%lu,"
                "\"expected_count\":%u,\"accepted_scans\":0,"
                "\"manual_attendance_entries\":0,"
                "\"request_id\":null,\"reason\":null,"
                "\"admin_session\":null}",
                static_cast<unsigned>(flagSession(event.flags)),
                static_cast<unsigned long>(event.epoch),
                static_cast<unsigned long>(event.seq),
                static_cast<unsigned>(event.a))) {
          batchOk = false;
          break;
        }
      } else if (automaticNoClassReview) {
        const char* action = event.a == ANCR_CONFIRM ? "confirm" :
            event.a == ANCR_UNDO ? "undo" : nullptr;
        if (action == nullptr || todayCas0Pointer == nullptr ||
            !appendFormat_(
                "{\"kind\":\"no_class_review\","
                "\"action\":\"%s\",\"scope\":\"session\","
                "\"source\":\"administrator\",\"session\":%u,"
                "\"target_sequence\":%lu,\"utc_epoch\":%lu,"
                "\"sequence\":%lu,\"request_id\":%s,"
                "\"reason\":%s,\"admin_session\":%s}",
                action, static_cast<unsigned>(flagSession(event.flags)),
                static_cast<unsigned long>(todayCas0Pointer->epoch),
                static_cast<unsigned long>(event.epoch),
                static_cast<unsigned long>(event.seq), auditFields.requestId,
                auditFields.reason, auditFields.adminSession)) {
          batchOk = false;
          break;
        }
      } else {
        const bool wholeDay = flagWholeDay(event.flags);
        if (!appendFormat_(
            "{\"kind\":\"no_class\",\"scope\":\"%s\"," 
            "\"operation\":\"%s\",\"source\":\"manual\","
            "\"session\":%u,\"utc_epoch\":%lu,\"sequence\":%lu," 
            "\"request_id\":%s,\"reason\":%s," 
            "\"admin_session\":%s}",
            wholeDay ? "day" : "session",
            event.type == EV_NOCLASS_SET ? "set" : "clear",
            static_cast<unsigned>(flagSession(event.flags)),
            static_cast<unsigned long>(event.epoch),
            static_cast<unsigned long>(event.seq), auditFields.requestId,
            auditFields.reason, auditFields.adminSession)) {
          batchOk = false;
          break;
        }
      }
      ++emitted;
    }
    wipe(raw, sizeof raw);
    if (!batchOk) break;
  }
  wipe(raw, sizeof raw);
  if (!batchOk) return false;
  if (position < total) {
    if (!appendFormat_("],\"next_cursor\":%lu}",
                       static_cast<unsigned long>(position))) return false;
  } else if (!appendLiteral_("],\"next_cursor\":null}")) {
    return false;
  }
  return !log_.faulted() && log_.count() == total &&
      log_.usesStorage(monthData_) && roster_.historyComplete() &&
      roster_.healthy();
}

bool Esp32AdminRosterHttpHandler::prepareDayView_(
    int32_t day, uint8_t session, bool& ready) {
  ready = false;
  const size_t maxSize = static_cast<size_t>(-1);
  if (!validateConfig(config_) || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy() ||
      !apiDaySupported(day) ||
      session > 1 || weekdayIndex(day) > 4) return false;

  const size_t recordCount = log_.count();
  const uint64_t generation = log_.openGeneration();
  if (recordCount > static_cast<size_t>(UINT32_MAX) ||
      recordCount > maxSize / REC_SIZE) return false;

  const bool changed = !dayCacheInitialized_ || dayCacheDay_ != day ||
      dayCacheSession_ != session || dayCacheRecordCount_ != recordCount ||
      dayCacheGeneration_ != generation;
  if (changed) {
    memset(&dayView_, 0, sizeof dayView_);
    memset(todayRevision_, 0, sizeof todayRevision_);
    memset(todayScanEpoch_, 0, sizeof todayScanEpoch_);
    memset(todayLastStatusEpoch_, 0, sizeof todayLastStatusEpoch_);
    memset(todayLastStatusSequence_, 0, sizeof todayLastStatusSequence_);
    dayView_.day = day;
    dayView_.session = session;
    dayCacheDay_ = day;
    dayCacheSession_ = session;
    dayCacheNextRecord_ = 0;
    dayCacheRecordCount_ = recordCount;
    dayCacheGeneration_ = generation;
    dayCacheInitialized_ = true;
    dayCacheReady_ = recordCount == 0;
  }

  // An operator mutation may only make a state-based no-op decision from a
  // complete, consistent replay. Bound each HTTP request to 128 log records.
  if (!dayCacheReady_) {
    uint8_t raw[kDaySummaryReadBatchRecords * REC_SIZE] = {};
    size_t processed = 0;
    while (dayCacheNextRecord_ < recordCount &&
           processed < kDaySummaryRecordBudget) {
      size_t take = recordCount - dayCacheNextRecord_;
      if (take > kDaySummaryReadBatchRecords)
        take = kDaySummaryReadBatchRecords;
      const size_t budgetLeft = kDaySummaryRecordBudget - processed;
      if (take > budgetLeft) take = budgetLeft;
      if (dayCacheNextRecord_ > maxSize / REC_SIZE ||
          !monthData_.read(dayCacheNextRecord_ * REC_SIZE, raw, take * REC_SIZE)) {
        wipe(raw, sizeof raw);
        dayCacheInitialized_ = false;
        dayCacheReady_ = false;
        return false;
      }
      for (size_t i = 0; i < take; ++i) {
        Event event = {};
        const size_t index = dayCacheNextRecord_;
        const uint64_t sequence = 1u + static_cast<uint64_t>(index);
        if (sequence > UINT32_MAX ||
            !decode(raw + i * REC_SIZE, event) ||
            event.seq != static_cast<uint32_t>(sequence)) {
          wipe(raw, sizeof raw);
          dayCacheInitialized_ = false;
          dayCacheReady_ = false;
          return false;
        }
        const bool matchingSession = flagSession(event.flags) == session;
        if (event.a >= 1 && event.a <= MAX_ID && matchingSession) {
          const uint16_t id = event.a;
          const bool matchingScan = event.type == EV_SCAN && event.epoch != 0 &&
              dayNumber(event.epoch, config_.params.utcOffsetMin) == day;
          const bool matchingManual =
              (event.type == EV_MARK || event.type == EV_TODAY_STATUS) &&
              event.b == day;
          if (matchingScan || matchingManual) {
            todayRevision_[id] = event.seq;
            if (matchingScan && todayScanEpoch_[id] == 0)
              todayScanEpoch_[id] = event.epoch;
            if (event.type == EV_TODAY_STATUS) {
              todayLastStatusEpoch_[id] = event.epoch;
              todayLastStatusSequence_[id] = event.seq;
            }
          }
        }
        applyEvent(dayView_, event, config_.params);
        ++dayCacheNextRecord_;
        ++processed;
      }
    }
    wipe(raw, sizeof raw);
    if (log_.faulted() || log_.count() != recordCount ||
        log_.openGeneration() != generation ||
        !roster_.historyComplete() || !roster_.healthy()) {
      dayCacheInitialized_ = false;
      dayCacheReady_ = false;
      return false;
    }
    dayCacheReady_ = dayCacheNextRecord_ == recordCount;
  }

  if (log_.faulted() || log_.count() != recordCount ||
      log_.openGeneration() != generation ||
      !roster_.historyComplete() || !roster_.healthy()) {
    dayCacheInitialized_ = false;
    dayCacheReady_ = false;
    return false;
  }
  ready = dayCacheReady_;
  return true;
}

bool Esp32AdminRosterHttpHandler::buildDaySummary_(
    int32_t day, uint8_t session, uint16_t after) {
  responseLength_ = 0;
  response_[0] = '\0';
  if (!validateConfig(config_) || log_.faulted() ||
      !log_.usesStorage(monthData_) ||
      !roster_.historyComplete() || !roster_.healthy() ||
      !apiDaySupported(day) ||
      session > 1 || weekdayIndex(day) > 4 ||
      !config_.tt.day[weekdayIndex(day)][session].enabled || after > MAX_ID)
    return false;

  bool viewReady = false;
  if (!prepareDayView_(day, session, viewReady)) return false;
  const size_t recordCount = dayCacheRecordCount_;
  const uint64_t generation = dayCacheGeneration_;
  if (!viewReady) {
    // The old student cursor is irrelevant until replay rebuilds a consistent
    // snapshot; callers repeat the same bounded GET to continue.
    return appendFormat_(
        "{\"ready\":false,\"day\":%ld,\"session\":%u,"
        "\"processed\":%lu,\"total\":%lu,"
        "\"closed\":false,\"coverage\":false,"
        "\"auto_no_class_pending\":false,\"auto_no_class_confirmed\":false,"
        "\"closure_state\":\"replaying\",\"expected_count\":0,"
        "\"log_generation\":\"%llu\",\"log_records\":%lu,"
        "\"items\":[],\"next_after\":null}",
        static_cast<long>(day), static_cast<unsigned>(session),
        static_cast<unsigned long>(dayCacheNextRecord_),
        static_cast<unsigned long>(recordCount),
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long>(recordCount));
  }

  if (!dayView_.closed) {
    const char* closureState = "unverified_clock";
    if (day < config_.goLiveDay) {
      closureState = "before_go_live_unverified";
    } else if (rtcClock_ != nullptr) {
      Ds3231Reading current = {0, false};
      if (rtcClock_->readUtcEpoch(0, current) == DS3231_OK &&
          !current.oscillatorStopped && current.epochUtc >= EPOCH_2020) {
        const int32_t today = dayNumber(current.epochUtc,
                                         config_.params.utcOffsetMin);
        if (today >= config_.goLiveDay && today <= MAX_SUPPORTED_DAY) {
          const int32_t rollingFirst = today - (MAX_TRACK_DAYS - 1);
          const int32_t activeFirst = rollingFirst > config_.goLiveDay
              ? rollingFirst : config_.goLiveDay;
          closureState = day < activeFirst
              ? "expired_unverified" : "pending";
        }
      }
    }
    return appendFormat_(
        "{\"ready\":true,\"day\":%ld,\"session\":%u,\"closed\":false,"
        "\"coverage\":false,\"auto_no_class_pending\":false,"
        "\"auto_no_class_confirmed\":false,\"closure_state\":\"%s\","
        "\"expected_count\":0,\"log_generation\":\"%llu\",\"log_records\":%lu,"
        "\"items\":[],\"next_after\":null}",
        static_cast<long>(day), static_cast<unsigned>(session), closureState,
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long>(recordCount));
  }

  uint16_t bitmapCount = 0;
  for (uint16_t id = 1; id <= MAX_ID; ++id)
    if (bitmapTest(dayView_.expected, id)) ++bitmapCount;
  if (bitmapCount != dayView_.expectedCount || dayView_.expectedCount > MAX_ID)
    return false;

  if (!appendFormat_(
      "{\"ready\":true,\"day\":%ld,\"session\":%u,\"closed\":true,"
      "\"coverage\":%s,\"auto_no_class_pending\":%s,"
      "\"auto_no_class_confirmed\":%s,\"closure_state\":\"%s\","
      "\"expected_count\":%u,\"log_generation\":\"%llu\",\"log_records\":%lu,"
      "\"items\":[",
      static_cast<long>(day), static_cast<unsigned>(session),
      dayView_.coverage ? "true" : "false",
      dayView_.autoNoClassPending ? "true" : "false",
      dayView_.autoNoClassConfirmed ? "true" : "false",
      dayView_.coverage ? "closed_covered" : "closed_unverified",
      static_cast<unsigned>(dayView_.expectedCount),
      static_cast<unsigned long long>(generation),
      static_cast<unsigned long>(recordCount))) return false;

  uint8_t emitted = 0;
  uint16_t lastId = after;
  bool hasMore = false;
  for (uint16_t id = static_cast<uint16_t>(after + 1u); id <= MAX_ID; ++id) {
    if (!bitmapTest(dayView_.expected, id)) continue;
    if (emitted >= kDaySummaryPageSize) {
      hasMore = true;
      break;
    }
    const Derived status = statusFor(dayView_, id, false, true);
    const char* label = nullptr;
    switch (status) {
      case D_NOCLASS: label = "no_class"; break;
      case D_PRESENT: label = "present"; break;
      case D_LATE: label = "late"; break;
      case D_EXCUSED: label = "excused"; break;
      case D_ABSENT: label = dayView_.coverage ? "absent" : nullptr; break;
      case D_UNVERIFIED: label = "unverified"; break;
      default: return false;
    }
    if (label == nullptr || (emitted != 0 && !appendByte_(',')) ||
        !appendFormat_("{\"student_id\":%u,\"status\":\"%s\"}",
                       static_cast<unsigned>(id), label)) return false;
    lastId = id;
    ++emitted;
  }
  if (hasMore) {
    if (!appendFormat_("],\"next_after\":%u}",
                       static_cast<unsigned>(lastId))) return false;
  } else if (!appendLiteral_("],\"next_after\":null}")) {
    return false;
  }
  return !log_.faulted() && log_.count() == recordCount &&
      log_.openGeneration() == generation && log_.usesStorage(monthData_) &&
      roster_.historyComplete() && roster_.healthy();
}

bool Esp32AdminRosterHttpHandler::prepareHistoricalRange_(
    int32_t fromDay, int32_t toDay, bool& ready) {
  ready = false;
  if (!validateConfig(config_) || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy() || !apiDaySupported(fromDay) ||
      !apiDaySupported(toDay) || fromDay > toDay ||
      toDay - fromDay > 6 || fromDay < config_.goLiveDay ||
      log_.count() > ATTENDANCE_MUTATION_MAX_LOG_RECORDS) return false;

  if (rtcClock_ == nullptr || clockHealth_ == nullptr ||
      !clockHealth_->clockTrusted()) return false;
  Ds3231Reading reading = {0, false};
  if (rtcClock_->readUtcEpoch(0, reading) != DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch()))
    return false;
  const int32_t today = dayNumber(reading.epochUtc, config_.params.utcOffsetMin);
  if (today < config_.goLiveDay || today > MAX_SUPPORTED_DAY || toDay > today)
    return false;
  const int32_t rollingFirst = today >= MAX_TRACK_DAYS - 1
      ? today - (MAX_TRACK_DAYS - 1) : 0;
  const int32_t activeFirst = rollingFirst > config_.goLiveDay
      ? rollingFirst : config_.goLiveDay;
  if (fromDay < activeFirst) return false;

  const size_t recordCount = log_.count();
  const uint64_t generation = log_.openGeneration();
  const bool changed = !historicalInitialized_ ||
      historicalFromDay_ != fromDay || historicalToDay_ != toDay ||
      historicalRecordCount_ != recordCount ||
      historicalGeneration_ != generation;
  if (changed) {
    memset(historicalViews_, 0, sizeof historicalViews_);
    memset(historicalColumns_, 0, sizeof historicalColumns_);
    historicalFromDay_ = fromDay;
    historicalToDay_ = toDay;
    historicalNextRecord_ = 0;
    historicalRecordCount_ = recordCount;
    historicalGeneration_ = generation;
    historicalColumnCount_ = 0;
    historicalReady_ = false;
    historicalInitialized_ = true;

    for (int32_t day = fromDay; day <= toDay; ++day) {
      const uint8_t weekday = weekdayIndex(day);
      if (weekday > 4) continue;
      for (uint8_t session = 0; session < 2; ++session) {
        if (!config_.tt.day[weekday][session].enabled) continue;
        if (historicalColumnCount_ >= 10) return false;
        const uint8_t viewIndex = static_cast<uint8_t>(
            (day - fromDay) * 2 + session);
        DayView& view = historicalViews_[viewIndex];
        view.day = day;
        view.session = session;
        HistoricalColumn& column =
            historicalColumns_[historicalColumnCount_++];
        column.day = day;
        column.session = session;
        column.viewIndex = viewIndex;
      }
    }
  }

  if (!historicalReady_) {
    uint8_t raw[kHistoricalReadBatchRecords * REC_SIZE] = {};
    size_t processed = 0;
    while (historicalNextRecord_ < historicalRecordCount_ &&
           processed < kHistoricalRecordBudget) {
      size_t take = historicalRecordCount_ - historicalNextRecord_;
      if (take > kHistoricalReadBatchRecords)
        take = kHistoricalReadBatchRecords;
      const size_t budgetLeft = kHistoricalRecordBudget - processed;
      if (take > budgetLeft) take = budgetLeft;
      if (take == 0 || historicalNextRecord_ >
              static_cast<size_t>(-1) / REC_SIZE ||
          !monthData_.read(historicalNextRecord_ * REC_SIZE,
                           raw, take * REC_SIZE)) {
        wipe(raw, sizeof raw);
        historicalInitialized_ = false;
        historicalReady_ = false;
        return false;
      }
      for (size_t i = 0; i < take; ++i) {
        Event event = {};
        const size_t index = historicalNextRecord_;
        const uint64_t expectedSeq = 1u + static_cast<uint64_t>(index);
        if (expectedSeq > UINT32_MAX ||
            !decode(raw + i * REC_SIZE, event) ||
            event.seq != static_cast<uint32_t>(expectedSeq)) {
          wipe(raw, sizeof raw);
          historicalInitialized_ = false;
          historicalReady_ = false;
          return false;
        }
        for (uint8_t c = 0; c < historicalColumnCount_; ++c)
          applyEvent(historicalViews_[historicalColumns_[c].viewIndex],
                     event, config_.params);
        ++historicalNextRecord_;
        ++processed;
      }
    }
    wipe(raw, sizeof raw);
    if (log_.faulted() || log_.count() != recordCount ||
        log_.openGeneration() != generation ||
        !roster_.historyComplete() || !roster_.healthy()) {
      historicalInitialized_ = false;
      historicalReady_ = false;
      return false;
    }
    if (historicalNextRecord_ == historicalRecordCount_) {
      // Compact the configured timetable columns to completed, retained CLOSE
      // snapshots only. A partial bitmap is never exposed as a writable row.
      uint8_t kept = 0;
      for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
        const HistoricalColumn column = historicalColumns_[c];
        const DayView& view = historicalViews_[column.viewIndex];
        if (!view.closed) continue;
        if (view.collecting || view.pi != payloadCount(MAX_ID) ||
            view.expectedCount > MAX_ID) {
          historicalInitialized_ = false;
          historicalReady_ = false;
          return false;
        }
        uint16_t bitmapCount = 0;
        for (uint16_t id = 1; id <= MAX_ID; ++id)
          if (bitmapTest(view.expected, id)) ++bitmapCount;
        if (bitmapCount != view.expectedCount) {
          historicalInitialized_ = false;
          historicalReady_ = false;
          return false;
        }
        historicalColumns_[kept++] = column;
      }
      historicalColumnCount_ = kept;
      historicalReady_ = true;
    }
  }

  if (log_.faulted() || log_.count() != recordCount ||
      log_.openGeneration() != generation ||
      !roster_.historyComplete() || !roster_.healthy()) {
    historicalInitialized_ = false;
    historicalReady_ = false;
    return false;
  }
  ready = historicalReady_;
  return true;
}

bool Esp32AdminRosterHttpHandler::buildHistoricalGrid_(
    int32_t fromDay, int32_t toDay, uint16_t after) {
  responseLength_ = 0;
  response_[0] = '\0';
  bool ready = false;
  if (!prepareHistoricalRange_(fromDay, toDay, ready)) return false;
  const uint64_t generation = historicalGeneration_;
  const uint32_t generationHigh = static_cast<uint32_t>(generation >> 32);
  const uint32_t generationLow = static_cast<uint32_t>(generation);
  if (!appendFormat_(
      "{\"ready\":%s,\"from\":%ld,\"to\":%ld,"
      "\"processed\":%lu,\"total\":%lu,"
      "\"device_id\":\"%012llx\","
      "\"log_generation_high\":%lu,\"log_generation_low\":%lu,"
      "\"log_records\":%lu,\"columns\":[",
      ready ? "true" : "false", static_cast<long>(fromDay),
      static_cast<long>(toDay),
      static_cast<unsigned long>(historicalNextRecord_),
      static_cast<unsigned long>(historicalRecordCount_),
      static_cast<unsigned long long>(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL),
      static_cast<unsigned long>(generationHigh),
      static_cast<unsigned long>(generationLow),
      static_cast<unsigned long>(historicalRecordCount_))) return false;

  if (!ready) {
    if (!appendLiteral_("],\"items\":[],\"next_after\":null}")) return false;
    return true;
  }

  for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
    const HistoricalColumn& column = historicalColumns_[c];
    const DayView& view = historicalViews_[column.viewIndex];
    if ((c != 0 && !appendByte_(',')) ||
        !appendFormat_("{\"day\":%ld,\"session\":%u,\"coverage\":%s}",
            static_cast<long>(column.day),
            static_cast<unsigned>(column.session),
            view.coverage ? "true" : "false")) return false;
  }
  if (!appendLiteral_("],\"items\":[")) return false;

  uint8_t emitted = 0;
  uint16_t lastId = after;
  bool hasMore = false;
  for (uint16_t id = static_cast<uint16_t>(after + 1u);
       id <= MAX_ID; ++id) {
    bool expectedSomewhere = false;
    for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
      const DayView& view = historicalViews_[historicalColumns_[c].viewIndex];
      if (bitmapTest(view.expected, id)) { expectedSomewhere = true; break; }
    }
    if (!expectedSomewhere) continue;
    if (emitted >= kHistoricalGridPageSize) { hasMore = true; break; }

    char name[ROSTER_NAME_MAX + 1] = {};
    const size_t nameLength = roster_.nameOf(id, name);
    if (nameLength == 0 || nameLength > ROSTER_NAME_MAX) {
      wipe(name, sizeof name);
      return false;
    }
    bool itemOk = (emitted == 0 || appendByte_(',')) &&
        appendFormat_("{\"student_id\":%u,\"name\":",
                      static_cast<unsigned>(id)) &&
        appendJsonString_(name, nameLength) &&
        appendLiteral_(",\"status\":\"");
    wipe(name, sizeof name);
    if (!itemOk) return false;

    for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
      const DayView& view = historicalViews_[historicalColumns_[c].viewIndex];
      char status = '-';
      if (bitmapTest(view.expected, id)) {
        switch (statusFor(view, id, false, true)) {
          case D_PRESENT: status = 'p'; break;
          case D_LATE: status = 'l'; break;
          case D_EXCUSED: status = 'e'; break;
          case D_ABSENT: status = 'a'; break;
          case D_UNVERIFIED: status = 'u'; break;
          case D_NOCLASS: status = 'n'; break;
          default: return false;
        }
      }
      if (!appendByte_(status)) return false;
    }
    if (!appendLiteral_("\",\"source\":\"")) return false;
    for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
      const DayView& view = historicalViews_[historicalColumns_[c].viewIndex];
      char source = '-';
      if (bitmapTest(view.expected, id)) {
        if (view.mark[id] != ST_CLEAR) source = 'm';
        else if (view.scan[id] == ST_PRESENT || view.scan[id] == ST_LATE)
          source = 'f';
      }
      if (!appendByte_(source)) return false;
    }
    if (!appendLiteral_("\",\"editable\":\"")) return false;
    for (uint8_t c = 0; c < historicalColumnCount_; ++c) {
      const DayView& view = historicalViews_[historicalColumns_[c].viewIndex];
      const bool editable = bitmapTest(view.expected, id) &&
          !(view.noClassDay || view.noClassSession);
      if (!appendByte_(editable ? '1' : '0')) return false;
    }
    if (!appendLiteral_("\"}")) return false;
    lastId = id;
    ++emitted;
  }
  if (hasMore) {
    if (!appendFormat_("],\"next_after\":%u}",
                       static_cast<unsigned>(lastId))) return false;
  } else if (!appendLiteral_("],\"next_after\":null}")) {
    return false;
  }
  return !log_.faulted() && log_.count() == historicalRecordCount_ &&
      log_.openGeneration() == historicalGeneration_ &&
      roster_.historyComplete() && roster_.healthy();
}

bool Esp32AdminRosterHttpHandler::writeAll_(
    NetworkClient& client, const uint8_t* bytes, size_t length, uint32_t started) {
  if ((bytes == nullptr && length != 0) ||
      static_cast<uint32_t>(millis() - started) >= kResponseWriteTimeoutMs) return false;
  const size_t kMaxChunkBytes = 64;
  size_t offset = 0;
  while (offset < length) {
    if (!client.connected() ||
        static_cast<uint32_t>(millis() - started) >= kResponseWriteTimeoutMs) return false;
    const size_t remaining = length - offset;
    const size_t chunk = remaining < kMaxChunkBytes ? remaining : kMaxChunkBytes;
    const size_t written = client.write(bytes + offset, chunk);
    if (written > chunk) return false;
    if (written == 0) {
      if (static_cast<uint32_t>(millis() - started) >= kResponseWriteTimeoutMs) return false;
      delay(1); continue;
    }
    offset += written;
    if (static_cast<uint32_t>(millis() - started) >= kResponseWriteTimeoutMs) return false;
  }
  return static_cast<uint32_t>(millis() - started) < kResponseWriteTimeoutMs;
}
bool Esp32AdminRosterHttpHandler::writeResponse_(
    WebServer& server, int status, const char* body, size_t length) {
  if ((body == nullptr && length != 0) ||
      (status != 200 && status != 400 && status != 401 && status != 404 &&
       status != 409 && status != 411 && status != 413 && status != 415 &&
       status != 422 && status != 423 && status != 503))
    return false;
  char header[320] = {};
  const int headerLength = snprintf(
      header, sizeof header,
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %lu\r\n"
      "Cache-Control: no-store, max-age=0\r\n"
      "Pragma: no-cache\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Referrer-Policy: no-referrer\r\n"
      "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
      "Connection: close\r\n\r\n",
      status, statusText(status), static_cast<unsigned long>(length));
  if (headerLength <= 0 || static_cast<size_t>(headerLength) >= sizeof header) {
    wipe(header, sizeof header); return false;
  }
  NetworkClient& client = server.client();
  const uint32_t started = millis();
  const bool headerOk = writeAll_(client, reinterpret_cast<const uint8_t*>(header),
                                  static_cast<size_t>(headerLength), started);
  const bool bodyOk = headerOk && writeAll_(client,
      reinterpret_cast<const uint8_t*>(body), length, started);
  wipe(header, sizeof header);
  return bodyOk;
}
void Esp32AdminRosterHttpHandler::sendError_(
    WebServer& server, int status, const char* error) {
  char payload[96] = {};
  const int length = snprintf(payload, sizeof payload, "{\"error\":\"%s\"}",
                              error != nullptr ? error : "request_rejected");
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, status, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  if (!sent) server.client().stop();
}
void Esp32AdminRosterHttpHandler::dispatchRosterMutation_(WebServer& server) {
  if (mutationBody_.rejected()) {
    sendError_(server, mutationBody_.httpStatus(), mutationBody_.error());
    mutationBody_.reset();
    return;
  }
  if (!mutationBody_.ready()) {
    sendError_(server, 400, "request_body_incomplete");
    mutationBody_.reset();
    return;
  }

  RosterMutationRequest request = {};
  const bool decoded = decodeRosterMutationRequest(
      mutationBody_.data(), mutationBody_.length(), request);
  mutationBody_.reset();
  if (!decoded) {
    sendError_(server, 400, "invalid_roster_mutation");
    return;
  }

  if (monotonicSeconds_ == nullptr || writer_ == nullptr || rtcClock_ == nullptr ||
      clockHealth_ == nullptr || storageWritesAuthorized_ == nullptr) {
    wipe(&request, sizeof request);
    sendError_(server, 503, "roster_mutation_unavailable");
    return;
  }

  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token);
    wipe(&request, sizeof request);
    sendError_(server, 401, "unauthorized");
    return;
  }
  const ApiResult authorized = sessions_.authorize(
      EP_ROSTER_SET, health_.state(), token, monotonicSeconds_());
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK) {
    wipe(&request, sizeof request);
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 400,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "roster_mutation_blocked_by_health" :
                                             "invalid_authorization");
    return;
  }

  if (!*storageWritesAuthorized_ || !clockHealth_->clockTrusted() ||
      (health_.state() != H_OK && health_.state() != H_SENSOR_FAULT) ||
      writer_->faulted() || log_.faulted() || !roster_.historyComplete()) {
    wipe(&request, sizeof request);
    sendError_(server, 423, "trusted_clock_or_storage_write_gate_unavailable");
    return;
  }

  Ds3231Reading reading = {0, false};
  const Ds3231Code rtcResult = rtcClock_->readUtcEpoch(0, reading);
  if (rtcResult != DS3231_OK || reading.oscillatorStopped ||
      reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch())) {
    wipe(&request, sizeof request);
    sendError_(server, 423, "rtc_not_trusted_no_roster_change_written");
    return;
  }

  RosterWriteOutcome outcome = {RWS_BLOCKED, RE_UNTRUSTED};
  if (request.operation == RMO_CREATE)
    outcome = writer_->create(request.id, request.name, request.nameLength,
                              reading.epochUtc);
  else if (request.operation == RMO_SET)
    outcome = writer_->set(request.id, request.name, request.nameLength,
                           reading.epochUtc);
  else if (request.operation == RMO_ACTIVATE)
    outcome = writer_->activate(request.id, reading.epochUtc);
  else if (request.operation == RMO_DEACTIVATE)
    outcome = writer_->deactivate(request.id, reading.epochUtc);

  const uint16_t id = request.id;
  wipe(&request, sizeof request);
  if (outcome.state == RWS_UNCERTAIN || outcome.state == RWS_BLOCKED ||
      writer_->faulted() || log_.faulted()) {
    applicationFaulted_ = true;
    sendError_(server, 503, outcome.state == RWS_UNCERTAIN
        ? "roster_commit_uncertain_power_cycle_required"
        : "roster_writer_blocked_power_cycle_required");
    return;
  }
  if (outcome.state == RWS_REJECTED) {
    const int status = outcome.rosterError == RE_UNKNOWN ? 404 :
        outcome.rosterError == RE_BACKDATED || outcome.rosterError == RE_FULL ||
        outcome.rosterError == RE_EXISTS ? 409 : 400;
    const char* error = outcome.rosterError == RE_EXISTS ? "student_id_already_exists" :
        outcome.rosterError == RE_BAD_ID ? "invalid_student_id" :
        outcome.rosterError == RE_BAD_NAME ? "invalid_student_name" :
        outcome.rosterError == RE_UNKNOWN ? "student_not_found" :
        outcome.rosterError == RE_FULL ? "roster_capacity_reached" :
        outcome.rosterError == RE_BACKDATED ? "roster_change_would_be_backdated" :
                                              "roster_change_rejected";
    sendError_(server, status, error);
    return;
  }

  char payload[112] = {};
  const int length = snprintf(payload, sizeof payload,
      "{\"id\":%u,\"active\":%s,\"changed\":%s}",
      static_cast<unsigned>(id), roster_.active(id) ? "true" : "false",
      outcome.state == RWS_COMMITTED ? "true" : "false");
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, 200, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  if (!sent) server.client().stop();
}

void Esp32AdminRosterHttpHandler::dispatchAttendanceMutation_(
    WebServer& server, const String& uri) {
  if (mutationBody_.rejected()) {
    sendError_(server, mutationBody_.httpStatus(), mutationBody_.error());
    mutationBody_.reset();
    return;
  }
  if (!mutationBody_.ready()) {
    sendError_(server, 400, "request_body_incomplete");
    mutationBody_.reset();
    return;
  }

  const bool markRequest = uri == "/api/v1/mark";
  const bool todayNoClassRequest = uri == kTodayNoClassUri;
  MarkReq mark = {};
  NoClassReq noClass = {};
  AttendanceMutationRequestId requestId = {{0, 0, 0, 0}};
  char todayNoClassRequestIdHex[17] = {};
  AdminMutationReason reason = AMR_NONE;
  const bool decoded = markRequest
      ? decodeMarkMutationRequest(mutationBody_.data(), mutationBody_.length(),
                                  mark, requestId, &reason)
      : decodeNoClassMutationRequest(mutationBody_.data(), mutationBody_.length(),
                                     noClass, requestId, &reason);
  mutationBody_.reset();
  if (!decoded) {
    wipe(&requestId, sizeof requestId);
    sendError_(server, 400, markRequest ? "invalid_mark_request" :
                                             "invalid_no_class_request");
    return;
  }
  if (todayNoClassRequest) {
    const int formatted = snprintf(todayNoClassRequestIdHex,
        sizeof todayNoClassRequestIdHex, "%04x%04x%04x%04x",
        static_cast<unsigned>(requestId.words[0]),
        static_cast<unsigned>(requestId.words[1]),
        static_cast<unsigned>(requestId.words[2]),
        static_cast<unsigned>(requestId.words[3]));
    if (formatted != 16) {
      wipe(&mark, sizeof mark);
      wipe(&noClass, sizeof noClass);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 500, "attendance_request_id_format_failed");
      return;
    }
  }

  if (monotonicSeconds_ == nullptr || attendanceWriter_ == nullptr ||
      rtcClock_ == nullptr || clockHealth_ == nullptr ||
      storageWritesAuthorized_ == nullptr || attendanceWriter_->faulted()) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 503, "attendance_mutation_unavailable");
    return;
  }

  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token);
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 401, "unauthorized");
    return;
  }
  const Ep endpoint = markRequest ? EP_MARK : EP_NOCLASS;
  const uint32_t monotonicNow = monotonicSeconds_();
  const ApiResult authorized = sessions_.authorize(
      endpoint, health_.state(), token, monotonicNow);
  const uint64_t sessionFingerprint = authorized.code == A_OK
      ? adminSessionFingerprint48(token) : 0;
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 400,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "manual_attendance_blocked_by_health" :
                                             "invalid_authorization");
    return;
  }

  if (sessionFingerprint == 0) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 503, "admin_session_audit_unavailable");
    return;
  }

  if (!*storageWritesAuthorized_ || !clockHealth_->clockTrusted() ||
      (health_.state() != H_OK && health_.state() != H_SENSOR_FAULT) ||
      applicationFaulted_ || attendanceWriter_->faulted() || log_.faulted() ||
      !roster_.historyComplete() || !roster_.healthy()) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423,
               "trusted_clock_or_storage_write_gate_unavailable");
    return;
  }

  Ds3231Reading reading = {0, false};
  const Ds3231Code rtcResult = rtcClock_->readUtcEpoch(0, reading);
  if (rtcResult != DS3231_OK || reading.oscillatorStopped ||
      reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch())) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "rtc_not_trusted_no_attendance_change_written");
    return;
  }

  const ApiCtx context = {reading.epochUtc, config_.params, config_.tt,
                                  MAX_ID, monotonicNow};
  const int32_t trustedDay = dayNumber(reading.epochUtc,
                                       config_.params.utcOffsetMin);
  if (!markRequest && !todayNoClassRequest &&
      uri == "/api/v1/no-class" && apiDaySupported(noClass.day) &&
      weekdayIndex(noClass.day) <= 4) {
    const uint8_t firstSession = noClass.wholeDay ? 0 : noClass.session;
    const uint8_t lastSession = noClass.wholeDay ? 1 : noClass.session;
    for (uint8_t session = firstSession; session <= lastSession; ++session) {
      bool viewReady = false;
      if (!prepareDayView_(noClass.day, session, viewReady) || !viewReady) {
        wipe(&mark, sizeof mark);
        wipe(&noClass, sizeof noClass);
        wipe(&requestId, sizeof requestId);
        sendError_(server, 503,
            "no_class_mutation_waiting_for_consistent_register_replay");
        return;
      }
      if (dayView_.autoNoClassPending) {
        wipe(&mark, sizeof mark);
        wipe(&noClass, sizeof noClass);
        wipe(&requestId, sizeof requestId);
        sendError_(server, 409,
            "automatic_no_class_must_be_reviewed_before_manual_no_class_change");
        return;
      }
      if (dayView_.autoNoClassConfirmed &&
          (noClass.wholeDay || noClass.set)) {
        wipe(&mark, sizeof mark);
        wipe(&noClass, sizeof noClass);
        wipe(&requestId, sizeof requestId);
        sendError_(server, 409,
            "confirmed_automatic_no_class_can_only_be_cleared_from_history");
        return;
      }
    }
  }
  if (todayNoClassRequest &&
      (noClass.wholeDay || noClass.day != trustedDay ||
       !apiDaySupported(trustedDay) || weekdayIndex(trustedDay) > 4 ||
       !config_.tt.day[weekdayIndex(trustedDay)][noClass.session].enabled ||
       reason == AMR_NONE)) {
    wipe(&mark, sizeof mark);
    wipe(&noClass, sizeof noClass);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 409, "today_no_class_must_target_current_configured_session");
    return;
  }
  if (todayNoClassRequest) {
    bool currentViewReady = false;
    if (!prepareDayView_(trustedDay, noClass.session, currentViewReady) ||
        !currentViewReady) {
      wipe(&mark, sizeof mark);
      wipe(&noClass, sizeof noClass);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 503, "today_no_class_waiting_for_consistent_register_replay");
      return;
    }
    // A whole-day cancellation has broader scope and can only be cleared
    // through the explicit History workflow, never from Today.
    if (dayView_.autoNoClassPending || dayView_.autoNoClassConfirmed) {
      wipe(&mark, sizeof mark);
      wipe(&noClass, sizeof noClass);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 409, "automatic_no_class_must_be_reviewed_in_today");
      return;
    }
    if (dayView_.noClassDay) {
      wipe(&mark, sizeof mark);
      wipe(&noClass, sizeof noClass);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 409, "whole_day_no_class_must_be_managed_in_history");
      return;
    }
  }
  const bool sameDayMark = markRequest && mark.day == trustedDay;
  const AttendanceWriteOutcome outcome = markRequest
      ? attendanceWriter_->markHistorical(mark, requestId, reason,
                                          sessionFingerprint, context, roster_)
      : attendanceWriter_->noClass(noClass, requestId, reason,
                                   sessionFingerprint, context);
  wipe(&mark, sizeof mark);
  wipe(&noClass, sizeof noClass);
  wipe(&requestId, sizeof requestId);

  if (outcome.state == AWS_UNCERTAIN || attendanceWriter_->faulted() ||
      log_.faulted()) {
    applicationFaulted_ = true;
    sendError_(server, 503,
        "attendance_commit_uncertain_power_cycle_and_replay_required");
    return;
  }
  if (outcome.state == AWS_LOOKUP_PENDING) {
    sendError_(server, 409,
        "attendance_request_lookup_pending_retry_same_request");
    return;
  }
  if (outcome.state == AWS_ID_REUSED) {
    sendError_(server, 409,
        "attendance_request_id_reused_for_different_operation");
    return;
  }
  if (outcome.state == AWS_BLOCKED) {
    sendError_(server, 423, "attendance_writer_blocked");
    return;
  }
  if (outcome.state == AWS_REJECTED) {
    const int status = outcome.code == A_NOTFOUND ? 404 :
        outcome.code == A_CONFLICT ? 409 :
        outcome.code == A_LOCKED ? 423 : 422;
    const char* error = reason == AMR_NONE && outcome.reason == R_BAD_FIELD
        ? "attendance_correction_reason_required" :
        outcome.reason == R_UNKNOWN_ID ? "student_not_found" :
        outcome.reason == R_BAD_FIELD ? "attendance_mutation_rejected" :
        outcome.reason == R_FUTURE ? "mark_day_is_in_future" :
        outcome.reason == R_NO_SUCH_SESSION ? "no_such_session" :
        outcome.reason == R_STATE
            ? (sameDayMark ? "same_day_changes_use_today_register"
                           : "attendance_mutation_blocked_by_state") :
                                    "attendance_mutation_rejected";
    sendError_(server, status, error);
    return;
  }

  const bool replayed = outcome.state == AWS_REPLAYED;
  char payload[160] = {};
  int length = 0;
  if (todayNoClassRequest) {
    length = snprintf(payload, sizeof payload,
        "{\"saved\":true,\"changed\":%s,\"sequence\":%lu,\"kind\":\"today_no_class\",\"request_id\":\"%s\"}",
        replayed ? "false" : "true",
        static_cast<unsigned long>(outcome.sequence), todayNoClassRequestIdHex);
  } else {
    length = snprintf(payload, sizeof payload,
        "{\"saved\":true,\"changed\":%s,\"sequence\":%lu,\"kind\":\"%s\"}",
        replayed ? "false" : "true",
        static_cast<unsigned long>(outcome.sequence),
        markRequest ? "mark" : "no_class");
  }
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, 200, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  wipe(todayNoClassRequestIdHex, sizeof todayNoClassRequestIdHex);
  if (!sent) server.client().stop();
}

void Esp32AdminRosterHttpHandler::dispatchAutomaticNoClassReviewMutation_(
    WebServer& server) {
  if (mutationBody_.rejected()) {
    sendError_(server, mutationBody_.httpStatus(), mutationBody_.error());
    mutationBody_.reset();
    return;
  }
  if (!mutationBody_.ready()) {
    sendError_(server, 400, "request_body_incomplete");
    mutationBody_.reset();
    return;
  }

  AutomaticNoClassReviewRequest request = {};
  AttendanceMutationRequestId requestId = {{0, 0, 0, 0}};
  const bool decoded = decodeAutomaticNoClassReviewRequest(
      mutationBody_.data(), mutationBody_.length(), request, requestId);
  mutationBody_.reset();
  if (!decoded) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 400, "invalid_automatic_no_class_review_request");
    return;
  }
  if (monotonicSeconds_ == nullptr || attendanceWriter_ == nullptr ||
      rtcClock_ == nullptr || clockHealth_ == nullptr ||
      storageWritesAuthorized_ == nullptr || attendanceWriter_->faulted()) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 503, "automatic_no_class_review_unavailable");
    return;
  }

  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token);
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 401, "unauthorized");
    return;
  }
  const uint32_t monotonicNow = monotonicSeconds_();
  const ApiResult authorized = sessions_.authorize(
      EP_NOCLASS, health_.state(), token, monotonicNow);
  const uint64_t sessionFingerprint = authorized.code == A_OK
      ? adminSessionFingerprint48(token) : 0;
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK || sessionFingerprint == 0) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 503,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "manual_attendance_blocked_by_health" :
                                             "admin_session_audit_unavailable");
    return;
  }

  if (!*storageWritesAuthorized_ || !clockHealth_->clockTrusted() ||
      (health_.state() != H_OK && health_.state() != H_SENSOR_FAULT) ||
      applicationFaulted_ || attendanceWriter_->faulted() || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy() || !validateConfig(config_)) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "trusted_clock_or_storage_write_gate_unavailable");
    return;
  }

  Ds3231Reading reading = {0, false};
  if (rtcClock_->readUtcEpoch(0, reading) != DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch())) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "rtc_not_trusted_no_attendance_change_written");
    return;
  }

  bool eligible = false;
  // Review is a historical resolution operation. Do not require that the
  // session is still enabled in today's timetable: a timetable edit must not
  // strand a previously committed provisional alert without Confirm/Undo.
  const bool sessionAddressable = apiDaySupported(request.day) &&
      weekdayIndex(request.day) <= 4 && request.day >= config_.goLiveDay;
  if (sessionAddressable) {
    bool viewReady = false;
    if (!prepareDayView_(request.day, request.session, viewReady) || !viewReady) {
      // A large append-only log may need several bounded replay requests before
      // its DayView is usable. Do not turn an incomplete replay into a stale
      // decision: the browser must retain and retry this exact request ID/body.
      wipe(&request, sizeof request);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 503,
          "automatic_no_class_review_waiting_for_consistent_register_replay");
      return;
    }
    eligible = dayView_.closed && dayView_.coverage &&
        dayView_.autoNoClassPending &&
        !dayView_.autoNoClassConfirmed &&
        dayView_.autoNoClassSequence == request.expectedAutoSequence &&
        dayView_.autoNoClassExpectedCount > 0 &&
        dayView_.noClassSession &&
        dayCacheRecordCount_ == request.expectedRecords &&
        dayCacheGeneration_ ==
            ((static_cast<uint64_t>(request.generationHigh) << 32) |
             static_cast<uint64_t>(request.generationLow));
  }

  const ApiCtx context = {reading.epochUtc, config_.params, config_.tt,
                          MAX_ID, monotonicNow};
  const AttendanceWriteOutcome outcome = attendanceWriter_->reviewAutomaticNoClass(
      request, requestId, sessionFingerprint, context, eligible);
  wipe(&request, sizeof request);
  wipe(&requestId, sizeof requestId);

  if (outcome.state == AWS_UNCERTAIN || attendanceWriter_->faulted() ||
      log_.faulted()) {
    applicationFaulted_ = true;
    sendError_(server, 503,
        "automatic_no_class_review_commit_uncertain_power_cycle_and_replay_required");
    return;
  }
  if (outcome.state == AWS_LOOKUP_PENDING) {
    sendError_(server, 409,
        "automatic_no_class_review_lookup_pending_retry_same_request");
    return;
  }
  if (outcome.state == AWS_ID_REUSED) {
    sendError_(server, 409,
        "attendance_request_id_reused_for_different_operation");
    return;
  }
  if (outcome.state == AWS_BLOCKED) {
    sendError_(server, 423, "attendance_writer_blocked");
    return;
  }
  if (outcome.state == AWS_REJECTED) {
    const int status = outcome.code == A_LOCKED ? 423 : 409;
    sendError_(server, status,
        outcome.reason == R_GEN_MISMATCH ? "automatic_no_class_snapshot_stale" :
        outcome.reason == R_BAD_FIELD ? "automatic_no_class_review_invalid" :
                                         "automatic_no_class_review_not_pending_or_stale");
    return;
  }

  const bool replayed = outcome.state == AWS_REPLAYED;
  char payload[160] = {};
  const int length = snprintf(payload, sizeof payload,
      "{\"saved\":true,\"changed\":%s,\"sequence\":%lu,"
      "\"kind\":\"automatic_no_class_review\"}",
      replayed ? "false" : "true",
      static_cast<unsigned long>(outcome.sequence));
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, 200, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  if (!sent) server.client().stop();
}

void Esp32AdminRosterHttpHandler::dispatchTodayStatusMutation_(WebServer& server) {
  if (mutationBody_.rejected()) {
    sendError_(server, mutationBody_.httpStatus(), mutationBody_.error());
    mutationBody_.reset();
    return;
  }
  if (!mutationBody_.ready()) {
    sendError_(server, 400, "request_body_incomplete");
    mutationBody_.reset();
    return;
  }

  TodayStatusMutationRequest request = {};
  AttendanceMutationRequestId requestId = {{0, 0, 0, 0}};
  const bool decoded = decodeTodayStatusMutationRequest(
      mutationBody_.data(), mutationBody_.length(), request, requestId);
  mutationBody_.reset();
  if (!decoded) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 400, "invalid_today_status_request");
    return;
  }
  if (monotonicSeconds_ == nullptr || attendanceWriter_ == nullptr ||
      rtcClock_ == nullptr || clockHealth_ == nullptr ||
      storageWritesAuthorized_ == nullptr || attendanceWriter_->faulted()) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 503, "today_status_mutation_unavailable");
    return;
  }

  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token);
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 401, "unauthorized");
    return;
  }
  const uint32_t monotonicNow = monotonicSeconds_();
  const ApiResult authorized = sessions_.authorize(
      EP_TODAY_STATUS, health_.state(), token, monotonicNow);
  const uint64_t sessionFingerprint = authorized.code == A_OK
      ? adminSessionFingerprint48(token) : 0;
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK || sessionFingerprint == 0) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 503,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "manual_attendance_blocked_by_health" :
                                              "admin_session_audit_unavailable");
    return;
  }

  // The trusted RTC, healthy storage/replay and existing health state remain
  // hard gates. Sensor-only failure is allowed for manual corrections, exactly
  // as the established attendance boundary permits.
  if (!*storageWritesAuthorized_ || !clockHealth_->clockTrusted() ||
      (health_.state() != H_OK && health_.state() != H_SENSOR_FAULT) ||
      applicationFaulted_ || attendanceWriter_->faulted() || log_.faulted() ||
      !log_.usesStorage(monthData_) || !roster_.historyComplete() ||
      !roster_.healthy()) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "trusted_clock_or_storage_write_gate_unavailable");
    return;
  }

  Ds3231Reading reading = {0, false};
  if (rtcClock_->readUtcEpoch(0, reading) != DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 && reading.epochUtc < log_.baselineEpoch())) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "rtc_not_trusted_no_attendance_change_written");
    return;
  }

  const ApiCtx context = {reading.epochUtc, config_.params, config_.tt,
                          MAX_ID, monotonicNow};
  bool eligible = false;
  uint32_t currentRevision = 0;
  uint8_t effectiveStatus = request.status;
  const int32_t trustedDay = dayNumber(reading.epochUtc,
                                       config_.params.utcOffsetMin);
  if (apiDaySupported(trustedDay) && trustedDay == request.day &&
      weekdayIndex(trustedDay) <= 4 &&
      config_.tt.day[weekdayIndex(trustedDay)][request.session].enabled) {
    bool viewReady = false;
    if (prepareDayView_(trustedDay, request.session, viewReady) && viewReady) {
      const uint16_t minute = minuteOfDay(reading.epochUtc,
                                          config_.params.utcOffsetMin);
      const Slot& slot = config_.tt.day[weekdayIndex(trustedDay)][request.session];
      const bool inActiveSession =
          sessionAt(config_.tt, config_.params, reading.epochUtc) == request.session &&
          hasStarted(slot, minute) && !hasEnded(slot, minute);
      const bool sessionEditable = !dayView_.closed &&
          !dayView_.noClassDay && !dayView_.noClassSession;
      if (request.studentId >= 1 && request.studentId <= MAX_ID) {
        currentRevision = todayRevision_[request.studentId];
        const bool expected = roster_.expected(request.studentId, trustedDay);
        const Derived current = statusFor(dayView_, request.studentId,
                                           true, hasStarted(slot, minute));
        bool requestedTransition = false;
        effectiveStatus = request.status;
        if (request.status == ST_PRESENT || request.status == ST_EXCUSED) {
          // A Pending row presents exactly two choices. "Present / Late" is
          // one intent; the terminal resolves the final code from its trusted
          // RTC and the configured late threshold at commit time.
          if (current == D_PENDING) {
            requestedTransition = true;
            if (request.status == ST_PRESENT) {
              effectiveStatus = isLate(slot, config_.params.lateMin, minute)
                  ? ST_LATE : ST_PRESENT;
            } else {
              effectiveStatus = ST_EXCUSED;
            }
          }
        } else if (request.status == ST_PENDING) {
          // The only legal Pending request from a non-Pending row is a
          // one-minute undo of the latest Today status decision. The row CAS
          // below also requires that no later scan or administrative event
          // has changed this student's revision.
          const uint8_t remaining =
              correctionSecondsRemaining(reading.epochUtc,
                                         todayLastStatusEpoch_[request.studentId]);
          const bool latestIsTodayDecision =
              todayLastStatusSequence_[request.studentId] != 0 &&
              todayLastStatusSequence_[request.studentId] == currentRevision;
          const bool hasUndoableManualStatus =
              dayView_.mark[request.studentId] == ST_PRESENT ||
              dayView_.mark[request.studentId] == ST_LATE ||
              dayView_.mark[request.studentId] == ST_EXCUSED;
          requestedTransition = current != D_PENDING && latestIsTodayDecision &&
              hasUndoableManualStatus && remaining > 0;
          effectiveStatus = ST_PENDING;
        }
        eligible = inActiveSession && sessionEditable && expected &&
            requestedTransition &&
            request.expectedRecords == dayCacheRecordCount_ &&
            request.generationHigh == static_cast<uint32_t>(dayCacheGeneration_ >> 32) &&
            request.generationLow == static_cast<uint32_t>(dayCacheGeneration_);
      }
    }
  }

  // The writer resolves a previously committed exact request before applying
  // today's eligibility, date, snapshot and row-revision gates. Thus a lost
  // HTTP response can be safely reconciled after midnight or a session close.
  const AttendanceWriteOutcome outcome = attendanceWriter_->todayStatus(
      request, requestId, sessionFingerprint, context, roster_, eligible,
      currentRevision, effectiveStatus);

  if (outcome.state == AWS_UNCERTAIN || attendanceWriter_->faulted() ||
      log_.faulted()) {
    applicationFaulted_ = true;
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 503,
        "today_status_commit_uncertain_retry_same_request_after_recovery");
    return;
  }
  if (outcome.state == AWS_LOOKUP_PENDING) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 409, "today_request_lookup_pending_retry_same_request");
    return;
  }
  if (outcome.state == AWS_ID_REUSED) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 409, "today_request_id_reused_for_different_payload");
    return;
  }
  if (outcome.state == AWS_BLOCKED) {
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, 423, "today_status_writer_blocked");
    return;
  }
  if (outcome.state == AWS_REJECTED) {
    const char* error = outcome.reason == R_GEN_MISMATCH
        ? "today_snapshot_or_revision_stale" :
        outcome.reason == R_STATE
            ? (request.status == ST_PENDING
                ? "today_correction_window_expired_or_row_changed"
                : "today_session_or_student_not_editable")
            : "today_status_mutation_rejected";
    wipe(&request, sizeof request);
    wipe(&requestId, sizeof requestId);
    sendError_(server, outcome.code == A_CONFLICT ? 409 : 422, error);
    return;
  }

  const bool replayed = outcome.state == AWS_REPLAYED;
  uint8_t committedStatus = effectiveStatus;
  if (replayed) {
    Event committed = {};
    if (outcome.sequence == 0 ||
        !log_.at(static_cast<size_t>(outcome.sequence - 1u), committed) ||
        committed.type != EV_TODAY_STATUS || committed.a != request.studentId ||
        committed.b != request.day || flagSession(committed.flags) != request.session) {
      applicationFaulted_ = true;
      wipe(&request, sizeof request);
      wipe(&requestId, sizeof requestId);
      sendError_(server, 503, "today_replayed_result_readback_unavailable");
      return;
    }
    committedStatus = flagStatus(committed.flags);
  }
  const char* statusName = committedStatus == ST_PRESENT ? "present" :
      committedStatus == ST_LATE ? "late" :
      committedStatus == ST_PENDING ? "pending" : "excused";
  char payload[320] = {};
  const int length = snprintf(payload, sizeof payload,
      "{\"saved\":true,\"changed\":%s,\"request_id\":\"%04x%04x%04x%04x\","
      "\"sequence\":%lu,\"revision\":%lu,\"status\":\"%s\","
      "\"log_records\":%lu,\"log_generation_high\":%lu,"
      "\"log_generation_low\":%lu}",
      replayed ? "false" : "true",
      static_cast<unsigned>(requestId.words[0]),
      static_cast<unsigned>(requestId.words[1]),
      static_cast<unsigned>(requestId.words[2]),
      static_cast<unsigned>(requestId.words[3]),
      static_cast<unsigned long>(outcome.sequence),
      static_cast<unsigned long>(outcome.sequence), statusName,
      static_cast<unsigned long>(log_.count()),
      static_cast<unsigned long>(log_.openGeneration() >> 32),
      static_cast<unsigned long>(log_.openGeneration()));
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, 200, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  wipe(&request, sizeof request);
  wipe(&requestId, sizeof requestId);
  if (!sent) server.client().stop();
}

void Esp32AdminRosterHttpHandler::dispatchHistoricalMarkMutation_(
    WebServer& server) {
  if (mutationBody_.rejected()) {
    sendError_(server, mutationBody_.httpStatus(), mutationBody_.error());
    mutationBody_.reset();
    return;
  }
  if (!mutationBody_.ready()) {
    sendError_(server, 400, "request_body_incomplete");
    mutationBody_.reset();
    return;
  }

  HistoricalMarkMutationRequest request = {};
  const bool decoded = decodeHistoricalMarkMutationRequest(
      mutationBody_.data(), mutationBody_.length(), request);
  mutationBody_.reset();
  if (!decoded) {
    wipe(&request, sizeof request);
    sendError_(server, 400, "invalid_historical_mark_request");
    return;
  }

  if (monotonicSeconds_ == nullptr || attendanceWriter_ == nullptr ||
      rtcClock_ == nullptr || clockHealth_ == nullptr ||
      storageWritesAuthorized_ == nullptr || attendanceWriter_->faulted()) {
    wipe(&request, sizeof request);
    sendError_(server, 503, "historical_mark_unavailable");
    return;
  }

  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token);
    wipe(&request, sizeof request);
    sendError_(server, 401, "unauthorized");
    return;
  }
  const uint32_t monotonicNow = monotonicSeconds_();
  const ApiResult authorized = sessions_.authorize(
      EP_MARK, health_.state(), token, monotonicNow);
  const uint64_t sessionFingerprint = authorized.code == A_OK
      ? adminSessionFingerprint48(token) : 0;
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK) {
    wipe(&request, sizeof request);
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 400,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "manual_attendance_blocked_by_health" :
                                             "invalid_authorization");
    return;
  }
  if (sessionFingerprint == 0) {
    wipe(&request, sizeof request);
    sendError_(server, 503, "admin_session_audit_unavailable");
    return;
  }
  if (!*storageWritesAuthorized_ || !clockHealth_->clockTrusted() ||
      (health_.state() != H_OK && health_.state() != H_SENSOR_FAULT) ||
      applicationFaulted_ || attendanceWriter_->faulted() || log_.faulted() ||
      !log_.usesStorage(monthData_) || !validateConfig(config_) ||
      !roster_.historyComplete() || !roster_.healthy()) {
    wipe(&request, sizeof request);
    sendError_(server, 423,
        "trusted_clock_or_storage_write_gate_unavailable");
    return;
  }

  const uint32_t expectedRecords = request.expectedRecords;
  const uint16_t changedStudentId = request.mark.id;
  const uint8_t changedStatus = request.mark.status;
  const bool overrideAcknowledged = request.overrideExisting;
  const uint64_t expectedGeneration =
      (static_cast<uint64_t>(request.generationHigh) << 32) |
      static_cast<uint64_t>(request.generationLow);
  if (!historicalInitialized_ || !historicalReady_) {
    wipe(&request, sizeof request);
    sendError_(server, 409, "historical_register_not_loaded_reload_register");
    return;
  }

  const HistoricalColumn* selectedColumn = nullptr;
  for (uint8_t i = 0; i < historicalColumnCount_; ++i) {
    const HistoricalColumn& column = historicalColumns_[i];
    if (column.day == request.mark.day &&
        column.session == request.mark.session) {
      selectedColumn = &column;
      break;
    }
  }
  const uint8_t selectedViewIndex = selectedColumn != nullptr
      ? selectedColumn->viewIndex : 0;
  bool historicalCellEligible = false;
  bool historicalOverrideRequired = false;
  if (selectedColumn != nullptr && request.mark.id >= 1 &&
      request.mark.id <= MAX_ID && request.mark.status >= ST_PRESENT &&
      request.mark.status <= ST_EXCUSED) {
    const DayView& historicalView = historicalViews_[selectedViewIndex];
    // The browser-supplied snapshot is not authoritative by itself. It must
    // match the exact snapshot from which this handler built its historical
    // projection. Otherwise a client could copy the *current* log count onto
    // a stale grid and bypass the compare-and-swap protection for a newly
    // cancelled session or changed attendance state. markIfSnapshot() checks
    // request identity first, so a previously committed exact retry still
    // replays even when this comparison is false.
    const bool loadedSnapshotMatches =
        historicalRecordCount_ <= ATTENDANCE_MUTATION_MAX_LOG_RECORDS &&
        expectedRecords == static_cast<uint32_t>(historicalRecordCount_) &&
        expectedGeneration != 0 &&
        expectedGeneration == historicalGeneration_;
    const bool expectedStudent = bitmapTest(
        historicalView.expected, request.mark.id);
    historicalCellEligible = loadedSnapshotMatches && historicalView.closed &&
        expectedStudent && !historicalView.noClassDay &&
        !historicalView.noClassSession;
    if (historicalCellEligible) {
      const Derived currentStatus = statusFor(
          historicalView, request.mark.id, false, true);
      historicalOverrideRequired =
          historicalView.mark[request.mark.id] != ST_CLEAR ||
          historicalView.scan[request.mark.id] == ST_PRESENT ||
          historicalView.scan[request.mark.id] == ST_LATE ||
          currentStatus == D_ABSENT;
    }
  }
  if (request.mark.status == ST_CLEAR ||
      request.mark.status < ST_PRESENT || request.mark.status > ST_EXCUSED) {
    wipe(&request, sizeof request);
    sendError_(server, 422, "historical_backfill_status_invalid");
    return;
  }

  Ds3231Reading reading = {0, false};
  if (rtcClock_->readUtcEpoch(0, reading) != DS3231_OK ||
      reading.oscillatorStopped || reading.epochUtc < EPOCH_2020 ||
      (log_.baselineEpoch() != 0 &&
       reading.epochUtc < log_.baselineEpoch())) {
    wipe(&request, sizeof request);
    sendError_(server, 423, "rtc_not_trusted_no_attendance_change_written");
    return;
  }
  const int32_t today = dayNumber(reading.epochUtc, config_.params.utcOffsetMin);
  const int32_t rollingFirst = today >= MAX_TRACK_DAYS - 1
      ? today - (MAX_TRACK_DAYS - 1) : 0;
  const int32_t activeFirst = rollingFirst > config_.goLiveDay
      ? rollingFirst : config_.goLiveDay;
  const bool historicalDateEligible = apiDaySupported(request.mark.day) &&
      request.mark.day >= activeFirst && request.mark.day <= today &&
      weekdayIndex(request.mark.day) <= 4 && request.mark.session <= 1 &&
      config_.tt.day[weekdayIndex(request.mark.day)][request.mark.session].enabled;

  const ApiCtx context = {reading.epochUtc, config_.params, config_.tt,
                          MAX_ID, monotonicNow};
  const AttendanceWriteOutcome outcome = attendanceWriter_->markIfSnapshot(
      request.mark, request.requestId, request.reason, sessionFingerprint,
      context, roster_,
      historicalCellEligible && historicalDateEligible &&
          (!historicalOverrideRequired || overrideAcknowledged),
      expectedRecords, expectedGeneration);
  wipe(&request, sizeof request);

  if (outcome.state == AWS_UNCERTAIN || attendanceWriter_->faulted() ||
      log_.faulted()) {
    applicationFaulted_ = true;
    sendError_(server, 503,
        "attendance_commit_uncertain_power_cycle_and_replay_required");
    return;
  }
  if (outcome.state == AWS_LOOKUP_PENDING) {
    sendError_(server, 409,
        "attendance_request_lookup_pending_retry_same_request");
    return;
  }
  if (outcome.state == AWS_ID_REUSED) {
    sendError_(server, 409,
        "attendance_request_id_reused_for_different_operation");
    return;
  }
  if (outcome.state == AWS_BLOCKED) {
    sendError_(server, 423, "attendance_writer_blocked");
    return;
  }
  if (outcome.state == AWS_REJECTED) {
    if (historicalCellEligible && historicalDateEligible &&
        historicalOverrideRequired && !overrideAcknowledged &&
        outcome.code == A_CONFLICT) {
      sendError_(server, 422,
          "historical_override_acknowledgement_required");
      return;
    }
    if (!historicalDateEligible) {
      sendError_(server, 422, "historical_day_outside_retained_class_range");
      return;
    }
    const int status = outcome.code == A_NOTFOUND ? 404 :
        outcome.code == A_CONFLICT ? 409 :
        outcome.code == A_LOCKED ? 423 : 422;
    const char* error = outcome.code == A_CONFLICT
        ? "historical_snapshot_stale_reload_register" :
        outcome.reason == R_UNKNOWN_ID ? "student_not_found" :
        outcome.reason == R_FUTURE ? "mark_day_is_in_future" :
        outcome.reason == R_NO_SUCH_SESSION ? "no_such_session" :
        outcome.reason == R_STATE ? "historical_mark_rejected_by_state" :
                                     "historical_mark_rejected";
    sendError_(server, status, error);
    return;
  }

  const bool replayed = outcome.state == AWS_REPLAYED;
  const uint32_t currentRecords = static_cast<uint32_t>(log_.count());
  const uint64_t currentGeneration = log_.openGeneration();
  const bool cleanSnapshotAdvance = selectedColumn != nullptr &&
      expectedRecords <= ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS &&
      currentGeneration == expectedGeneration &&
      currentRecords == expectedRecords + 3u;
  if (cleanSnapshotAdvance && selectedColumn != nullptr) {
    // Keep the already-replayed range current for the next sequential batch
    // operation. Only the corrected cell changes in this three-record group.
    historicalViews_[selectedViewIndex].mark[changedStudentId] =
        changedStatus;
    historicalRecordCount_ = currentRecords;
    historicalGeneration_ = currentGeneration;
  } else {
    historicalInitialized_ = false;
    historicalReady_ = false;
  }
  char payload[224] = {};
  const int length = snprintf(payload, sizeof payload,
      "{\"saved\":true,\"changed\":%s,\"sequence\":%lu,"
      "\"log_records\":%lu,\"log_generation_high\":%lu,"
      "\"log_generation_low\":%lu,\"snapshot_advanced_cleanly\":%s}",
      replayed ? "false" : "true",
      static_cast<unsigned long>(outcome.sequence),
      static_cast<unsigned long>(currentRecords),
      static_cast<unsigned long>(currentGeneration >> 32),
      static_cast<unsigned long>(currentGeneration),
      cleanSnapshotAdvance ? "true" : "false");
  const bool sent = length > 0 && static_cast<size_t>(length) < sizeof payload &&
      writeResponse_(server, 200, payload, static_cast<size_t>(length));
  wipe(payload, sizeof payload);
  if (!sent) server.client().stop();
}

bool Esp32AdminRosterHttpHandler::handle(
    WebServer& server, HTTPMethod method, const String& uri) {
  if (!canHandle(server, method, uri)) return false;
  if (method == HTTP_POST && uri == kRosterUri) {
    dispatchRosterMutation_(server);
    return true;
  }
  if (method == HTTP_POST && uri == kTodayStatusUri) {
    dispatchTodayStatusMutation_(server);
    return true;
  }
  if (method == HTTP_POST && uri == kTodayNoClassUri) {
    dispatchAttendanceMutation_(server, uri);
    return true;
  }
  if (method == HTTP_POST && uri == kTodayNoClassReviewUri) {
    dispatchAutomaticNoClassReviewMutation_(server);
    return true;
  }
  if (method == HTTP_POST && uri == kHistoricalMarkUri) {
    dispatchHistoricalMarkMutation_(server);
    return true;
  }
  if (method == HTTP_POST &&
      (uri == "/api/v1/mark" || uri == "/api/v1/no-class")) {
    dispatchAttendanceMutation_(server, uri);
    return true;
  }
  if (monotonicSeconds_ == nullptr) {
    sendError_(server, 503, "monotonic_clock_unavailable"); return true;
  }
  String authorization = server.header("Authorization");
  uint8_t token[16] = {};
  const bool tokenDecoded = decodeBearer(authorization, token);
  authorization = "";
  if (!tokenDecoded) {
    wipe(token, sizeof token); sendError_(server, 401, "unauthorized"); return true;
  }
  const bool rosterRequest = uri == kRosterUri;
  const bool todayRequest = uri == kTodayUri;
  const bool pendingAutomaticNoClassRequest = uri == kPendingAutoNoClassUri;
  const bool timetableRequest = uri == kTimetableUri;
  const bool daySummaryRequest = uri == kDaySummaryUri;
  const bool dayAdjustmentsRequest = uri == kDayAdjustmentsUri;
  const bool historicalGridRequest = uri == kHistoricalGridUri;
  const Ep endpoint = rosterRequest ? EP_ROSTER_GET :
      timetableRequest ? EP_CONFIG_GET : EP_DAY;
  const ApiResult authorized = sessions_.authorize(
      endpoint, health_.state(), token, monotonicSeconds_());
  wipe(token, sizeof token);
  if (sessions_.faulted()) applicationFaulted_ = true;
  if (authorized.code != A_OK) {
    sendError_(server, authorized.code == A_UNAUTH ? 401 :
                       authorized.code == A_LOCKED ? 423 : 400,
               authorized.code == A_UNAUTH ? "unauthorized" :
               authorized.code == A_LOCKED ? "read_operation_blocked" :
                                              "invalid_authorization");
    return true;
  }
  bool built = false;
  if (rosterRequest) {
    uint16_t after = 0;
    if (!parseAfter_(server, after)) {
      sendError_(server, 400, "invalid_cursor"); return true;
    }
    if (!roster_.historyComplete() || !roster_.healthy()) {
      sendError_(server, 503, "roster_history_unavailable"); return true;
    }
    built = buildPage_(after);
  } else if (pendingAutomaticNoClassRequest) {
    size_t after = 0;
    if (!parsePendingNoClassQuery_(server, after)) {
      sendError_(server, 400, "invalid_automatic_no_class_pending_cursor"); return true;
    }
    built = buildPendingAutomaticNoClass_(after);
  } else if (todayRequest) {
    uint8_t session = 0;
    uint16_t after = 0;
    if (!parseTodayQuery_(server, session, after)) {
      sendError_(server, 400, "invalid_today_query"); return true;
    }
    built = buildToday_(session, after);
  } else if (timetableRequest) {
    if (server.args() != 0) {
      sendError_(server, 400, "unexpected_query"); return true;
    }
    built = buildTimetable_();
  } else if (daySummaryRequest) {
    int32_t day = 0;
    uint8_t session = 0;
    uint16_t after = 0;
    if (!parseDaySummaryQuery_(server, day, session, after)) {
      sendError_(server, 400, "invalid_day_summary_query"); return true;
    }
    if (!apiDaySupported(day)) {
      sendError_(server, 400, "unsupported_day"); return true;
    }
    if (!validateConfig(config_) || session > 1 || weekdayIndex(day) > 4 ||
        !config_.tt.day[weekdayIndex(day)][session].enabled) {
      sendError_(server, 400, "invalid_session"); return true;
    }
    if (log_.faulted() || !roster_.historyComplete() || !roster_.healthy()) {
      sendError_(server, 503, "session_summary_unavailable"); return true;
    }
    built = buildDaySummary_(day, session, after);
  } else if (dayAdjustmentsRequest) {
    int32_t day = 0;
    uint8_t session = 0;
    uint32_t cursor = 0;
    if (!parseDayScansQuery_(server, day, session, cursor)) {
      sendError_(server, 400, "invalid_adjustments_query"); return true;
    }
    if (!apiDaySupported(day) ||
        weekdayIndex(day) > 4) {
      sendError_(server, 400, "unsupported_day_or_weekend"); return true;
    }
    if (!validateConfig(config_) || session > 1 || log_.faulted() ||
        !roster_.historyComplete() || !roster_.healthy()) {
      sendError_(server, 503, "adjustment_history_unavailable"); return true;
    }
    if (log_.count() > UINT32_MAX || cursor > log_.count()) {
      sendError_(server, 400, "invalid_cursor"); return true;
    }
    built = buildDayAdjustments_(day, session, cursor);
  } else if (historicalGridRequest) {
    int32_t fromDay = 0, toDay = 0;
    uint16_t after = 0;
    if (!parseHistoricalGridQuery_(server, fromDay, toDay, after)) {
      sendError_(server, 400, "invalid_historical_grid_query"); return true;
    }
    if (!apiDaySupported(fromDay) || !apiDaySupported(toDay) ||
        fromDay > toDay || toDay - fromDay > 6 ||
        fromDay < config_.goLiveDay) {
      sendError_(server, 400, "historical_range_out_of_bounds"); return true;
    }
    if (rtcClock_ == nullptr || clockHealth_ == nullptr ||
        !clockHealth_->clockTrusted()) {
      sendError_(server, 423, "historical_range_requires_trusted_clock");
      return true;
    }
    built = buildHistoricalGrid_(fromDay, toDay, after);
  } else {
    int32_t day = 0;
    uint8_t session = 0;
    uint32_t cursor = 0;
    if (!parseDayScansQuery_(server, day, session, cursor)) {
      sendError_(server, 400, "invalid_day_query"); return true;
    }
    if (!apiDaySupported(day)) {
      sendError_(server, 400, "unsupported_day"); return true;
    }
    if (!validateConfig(config_) || session > 1 || weekdayIndex(day) > 4 ||
        !config_.tt.day[weekdayIndex(day)][session].enabled) {
      sendError_(server, 400, "invalid_session"); return true;
    }
    if (log_.faulted() || !roster_.historyComplete() || !roster_.healthy()) {
      sendError_(server, 503, "scan_log_unavailable"); return true;
    }
    if (log_.count() > UINT32_MAX || cursor > log_.count()) {
      sendError_(server, 400, "invalid_cursor"); return true;
    }
    built = buildDayScans_(day, session, cursor);
  }
  if (!built) {
    wipe(response_, sizeof response_); responseLength_ = 0;
    sendError_(server, 503, rosterRequest
        ? "roster_response_unavailable" :
        todayRequest ? "today_register_unavailable_or_untrusted" :
        pendingAutomaticNoClassRequest ? "automatic_no_class_pending_unavailable" :
        timetableRequest ? "timetable_unavailable" :
        daySummaryRequest ? "session_summary_unavailable" :
        dayAdjustmentsRequest ? "adjustment_history_unavailable" :
        historicalGridRequest ? "historical_grid_unavailable_or_untrusted" :
                                "scan_log_unavailable");
    return true;
  }
  const bool sent = writeResponse_(server, 200, response_, responseLength_);
  wipe(response_, sizeof response_); responseLength_ = 0;
  if (!sent) server.client().stop();
  return true;
}
}  // namespace att
#endif  // ESP_PLATFORM
