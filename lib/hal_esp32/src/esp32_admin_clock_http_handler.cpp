// HTTP adapters are production-only; native tests exercise their core boundaries.
#if defined(ESP_PLATFORM)
#include "esp32_admin_clock_http_handler.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>

namespace att {
namespace {

const char kClockUri[] = "/api/v1/clock";

void wipe(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

bool decodeBearer(const String& header, uint8_t token[16]) {
  static const char prefix[] = "Bearer ";
  if (token == nullptr || header.length() != 39) return false;
  for (size_t i = 0; i < sizeof(prefix) - 1; ++i)
    if (header[i] != prefix[i]) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char highChar = header[7 + i * 2];
    const char lowChar = header[8 + i * 2];
    const int high = highChar >= '0' && highChar <= '9'
        ? highChar - '0'
        : highChar >= 'a' && highChar <= 'f'
            ? highChar - 'a' + 10 : -1;
    const int low = lowChar >= '0' && lowChar <= '9'
        ? lowChar - '0'
        : lowChar >= 'a' && lowChar <= 'f'
            ? lowChar - 'a' + 10 : -1;
    if (high < 0 || low < 0) {
      wipe(token, 16);
      return false;
    }
    token[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

const char* healthText(HState state) {
  switch (state) {
    case H_OK: return "ok";
    case H_UNPROVISIONED: return "unprovisioned";
    case H_CLOCK_FAULT: return "clock_fault";
    case H_STORAGE_FAULT: return "storage_fault";
    case H_CONFIG_FAULT: return "config_fault";
    case H_SENSOR_FAULT: return "sensor_fault";
    default: return "unknown";
  }
}

}  // namespace

Esp32AdminClockHttpHandler::Esp32AdminClockHttpHandler(
    ClockHealthOwner& clockOwner, LogStore& log, MonthSet& months,
    MonotonicSecondsSource monotonicSeconds, CurrentHealthSource currentHealth,
    bool& applicationFaulted)
    : clockOwner_(clockOwner), log_(log), months_(months),
      monotonicSeconds_(monotonicSeconds), currentHealth_(currentHealth),
      applicationFaulted_(applicationFaulted), body_{0}, expectedLength_(0),
      receivedLength_(0), receiving_(false), bodyReady_(false),
      rejected_(false), pendingStatus_(0), pendingError_(nullptr) {}

bool Esp32AdminClockHttpHandler::canHandle(
    WebServer& server, HTTPMethod method, const String& uri) {
  (void)server;
  return method == HTTP_POST && uri == kClockUri;
}

bool Esp32AdminClockHttpHandler::canRaw(
    WebServer& server, const String& uri) {
  return server.method() == HTTP_POST && uri == kClockUri;
}

bool Esp32AdminClockHttpHandler::canUpload(
    WebServer& server, const String& uri) {
  return server.method() == HTTP_POST && uri == kClockUri;
}

bool Esp32AdminClockHttpHandler::contentLength_(
    const String& value, size_t& out) const {
  if (value.length() == 0) return false;
  size_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return false;
    const size_t digit = static_cast<size_t>(c - '0');
    if (digit > CLOCK_CONFIRM_REQUEST_MAX_BYTES ||
        parsed > (CLOCK_CONFIRM_REQUEST_MAX_BYTES - digit) / 10u)
      return false;
    parsed = parsed * 10u + digit;
  }
  if (parsed == 0) return false;
  out = parsed;
  return true;
}

void Esp32AdminClockHttpHandler::clearBody_() {
  wipe(body_, sizeof body_);
  expectedLength_ = 0;
  receivedLength_ = 0;
  receiving_ = false;
  bodyReady_ = false;
}

void Esp32AdminClockHttpHandler::reject_(int status, const char* error) {
  if (!rejected_) {
    pendingStatus_ = status;
    pendingError_ = error;
  }
  rejected_ = true;
  clearBody_();
}

void Esp32AdminClockHttpHandler::raw(
    WebServer& server, const String& uri, HTTPRaw& rawData) {
  if (uri != kClockUri) return;
  if (rawData.status != RAW_START && rejected_) return;

  if (rawData.status == RAW_START) {
    clearBody_();
    rejected_ = false;
    pendingStatus_ = 0;
    pendingError_ = nullptr;

    if (server.header("Transfer-Encoding").length() != 0) {
      reject_(400, "transfer_encoding_not_supported");
      return;
    }
    if (!server.header("Content-Type").equalsIgnoreCase("application/json")) {
      reject_(415, "application_json_required");
      return;
    }
    if (!contentLength_(server.header("Content-Length"), expectedLength_)) {
      reject_(411, "valid_content_length_required");
      return;
    }
    if (server.clientContentLength() != static_cast<int>(expectedLength_)) {
      reject_(400, "content_length_mismatch");
      return;
    }
    receiving_ = true;
    return;
  }

  if (rawData.status == RAW_WRITE) {
    if (!receiving_ || rawData.currentSize == 0 ||
        rawData.totalSize != receivedLength_ + rawData.currentSize ||
        rawData.currentSize > expectedLength_ - receivedLength_) {
      reject_(400, "invalid_request_chunk");
      return;
    }
    memcpy(body_ + receivedLength_, rawData.buf, rawData.currentSize);
    receivedLength_ += rawData.currentSize;
    return;
  }

  if (rawData.status == RAW_END) {
    if (!receiving_ || receivedLength_ != expectedLength_) {
      reject_(400, "request_body_incomplete");
      return;
    }
    body_[receivedLength_] = '\0';
    receiving_ = false;
    bodyReady_ = true;
    return;
  }

  if (rawData.status == RAW_ABORTED) reject_(400, "request_body_incomplete");
}

void Esp32AdminClockHttpHandler::upload(
    WebServer& server, const String& uri, HTTPUpload& uploadData) {
  (void)server;
  if (uri == kClockUri && uploadData.status == UPLOAD_FILE_START)
    reject_(415, "multipart_file_upload_not_supported");
}

void Esp32AdminClockHttpHandler::sendJson_(
    WebServer& server, int status, const char* json) {
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; frame-ancestors 'none'");
  server.send(status, "application/json", json != nullptr ? json : "{}");
}

void Esp32AdminClockHttpHandler::sendError_(
    WebServer& server, int status, const char* error) {
  char payload[96] = {};
  const int length = snprintf(payload, sizeof payload,
                              "{\"error\":\"%s\"}",
                              error != nullptr ? error : "request_rejected");
  if (length <= 0 || static_cast<size_t>(length) >= sizeof payload) {
    sendJson_(server, 500, "{\"error\":\"response_error\"}");
    return;
  }
  sendJson_(server, status, payload);
  wipe(payload, sizeof payload);
}

void Esp32AdminClockHttpHandler::dispatch_(WebServer& server) {
  if (rejected_) {
    sendError_(server, pendingStatus_ != 0 ? pendingStatus_ : 400,
               pendingError_);
    return;
  }
  if (!bodyReady_) {
    sendError_(server, 400, "request_body_incomplete");
    return;
  }

  ClockConfirmRequest request = {0, false, false, 0, 0, 0};
  if (decodeClockConfirmRequest(body_, receivedLength_, request) != CCRD_OK) {
    clearBody_();
    sendError_(server, 400, "invalid_clock_request");
    return;
  }
  clearBody_();

  uint8_t token[16] = {};
  const String authorization = server.header("Authorization");
  if (!decodeBearer(authorization, token)) {
    wipe(token, sizeof token);
    sendError_(server, 401, "unauthorized");
    return;
  }

  HealthInputs currentHealth = {};
  if (currentHealth_ == nullptr || !currentHealth_(currentHealth)) {
    wipe(token, sizeof token);
    sendError_(server, 503, "clock_health_unavailable");
    return;
  }

  const ClockHealthDispatchResult result = clockOwner_.confirm(
      token, monotonicSeconds_ != nullptr ? monotonicSeconds_() : 0,
      log_, months_, request, currentHealth);
  wipe(token, sizeof token);

  if (!result.dispatched) {
    sendError_(server, 503, "clock_owner_unavailable");
    return;
  }
  if (result.admin.code == ACDC_AUTHORIZATION_DENIED) {
    const int status = result.admin.authorization.code == A_UNAUTH ? 401 :
        result.admin.authorization.code == A_LOCKOUT ? 429 : 423;
    sendError_(server, status,
               status == 401 ? "unauthorized" : "clock_operation_locked");
    return;
  }

  if (result.clockAccepted) {
    char payload[192] = {};
    const int length = snprintf(
        payload, sizeof payload,
        "{\"clock_trusted\":true,\"health\":\"%s\","
        "\"confirmed_utc_epoch\":%lu,\"log_baseline_utc\":%lu}",
        healthText(result.healthState),
        static_cast<unsigned long>(result.admin.confirmation.confirmedUtcEpoch),
        static_cast<unsigned long>(result.admin.confirmation.replayBaselineEpoch));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof payload)
      sendError_(server, 500, "response_error");
    else
      sendJson_(server, 200, payload);
    wipe(payload, sizeof payload);
    return;
  }

  const ClockConfirmResult& confirmation = result.admin.confirmation;
  if (confirmation.code == CCF_CONFIRMATION_REQUIRED) {
    sendError_(server, 409,
        confirmation.action == CA_NEED_CONFIRM_BACK
            ? "confirm_backward_required" : "confirm_jump_required");
    return;
  }
  if (confirmation.code == CCF_BLOCKED_WINDOW) {
    sendError_(server, 423, "clock_change_blocked_during_session_window");
    return;
  }
  if (confirmation.code == CCF_BAD_INPUT) {
    sendError_(server, 400, "invalid_clock_value");
    return;
  }
  if (confirmation.code == CCF_STORAGE_OWNER_READ_ONLY ||
      confirmation.code == CCF_STORAGE_OWNER_NOT_READY ||
      confirmation.code == CCF_STORAGE_OWNER_MISMATCH) {
    sendError_(server, 423, "clock_storage_write_gate_closed");
    return;
  }
  if (confirmation.code == CCF_RTC_READ_FAILED) {
    sendError_(server, 503, "rtc_read_failed_no_clock_change_confirmed");
    return;
  }
  if (confirmation.code == CCF_REQUEST_ID_INVALID ||
      confirmation.code == CCF_REQUEST_ID_REUSED) {
    sendError_(server, 400, "clock_request_id_invalid_or_reused");
    return;
  }
  if (confirmation.code == CCF_REQUEST_PENDING) {
    sendError_(server, 409, "another_clock_request_is_pending_use_its_request_id");
    return;
  }
  if (confirmation.code == CCF_REPLAY_CLOCK_UNTRUSTED) {
    sendError_(server, 423, "completed_clock_request_no_longer_matches_rtc_log");
    return;
  }

  if (clockOwner_.serviceBlocked()) {
    // An append/RTC transaction may have partially or fully committed. Stop
    // every ordinary service in this boot; never invite an HTTP retry.
    applicationFaulted_ = true;
    sendError_(server, 503, "clock_outcome_uncertain_power_cycle_required");
    return;
  }

  sendError_(server,
      confirmation.code == CCF_HEALTH_BLOCKED ? 423 : 503,
      confirmation.code == CCF_HEALTH_BLOCKED
          ? "clock_operation_blocked_by_health" : "clock_confirmation_unavailable");
}

bool Esp32AdminClockHttpHandler::handle(
    WebServer& server, HTTPMethod method, const String& uri) {
  if (method != HTTP_POST || uri != kClockUri) return false;
  dispatch_(server);
  return true;
}

}  // namespace att
#endif  // ESP_PLATFORM
