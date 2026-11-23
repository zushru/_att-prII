#if defined(ESP_PLATFORM)

#include "esp32_recovery_http_handler.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>

#include "recovery_http_request.h"

namespace att {
namespace {

const char kPreviewUri[] = "/api/v1/recovery/preview";
const char kPendingUri[] = "/api/v1/recovery/pending";
const char kPrepareUri[] = "/api/v1/recovery/prepare";
const char kResumeUri[] = "/api/v1/recovery/resume";

void wipe(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

const char* recoveryStatusName(RecoveryStatus status) {
  switch (status) {
    case RECOVERY_COMPLETE: return "complete";
    case RECOVERY_NO_PENDING: return "no_pending";
    case RECOVERY_AUTH_REQUIRED: return "auth_required";
    case RECOVERY_READ_ONLY: return "read_only";
    case RECOVERY_INVALID_REQUEST: return "invalid_request";
    case RECOVERY_AMBIGUOUS_PENDING: return "ambiguous_pending";
    case RECOVERY_PENDING_EXISTS: return "pending_exists";
    case RECOVERY_BACKEND_FAILURE: return "backend_failure";
    case RECOVERY_SOURCE_CHANGED: return "source_changed";
    case RECOVERY_ARCHIVE_FAILURE: return "archive_failure";
    case RECOVERY_ACTIVE_MISMATCH: return "active_mismatch";
    case RECOVERY_PREVIEW_READY: return "preview_ready";
    case RECOVERY_STALE_PREVIEW: return "stale_preview";
    case RECOVERY_PRESENCE_REQUIRED: return "presence_required";
    default: return "unknown";
  }
}

int recoveryHttpStatus(RecoveryStatus status) {
  switch (status) {
    case RECOVERY_COMPLETE:
    case RECOVERY_PREVIEW_READY:
    case RECOVERY_NO_PENDING:
      return 200;
    case RECOVERY_AUTH_REQUIRED:
      return 401;
    case RECOVERY_PRESENCE_REQUIRED:
    case RECOVERY_READ_ONLY:
      return 423;
    case RECOVERY_INVALID_REQUEST:
      return 422;
    case RECOVERY_STALE_PREVIEW:
    case RECOVERY_AMBIGUOUS_PENDING:
    case RECOVERY_PENDING_EXISTS:
    case RECOVERY_SOURCE_CHANGED:
      return 409;
    case RECOVERY_BACKEND_FAILURE:
    case RECOVERY_ARCHIVE_FAILURE:
    case RECOVERY_ACTIVE_MISMATCH:
      return 503;
    default:
      return 400;
  }
}

void encodeHex(const uint8_t* source, size_t length, char* destination) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    destination[i * 2u] = digits[source[i] >> 4];
    destination[i * 2u + 1u] = digits[source[i] & 0x0fu];
  }
  destination[length * 2u] = '\0';
}

}  // namespace

Esp32RecoveryHttpHandler::Esp32RecoveryHttpHandler(
    Esp32RecoveryService& recovery, AdminSessionBoundary& sessions,
    IRecoveryPhysicalPresence& presence,
    MonotonicSecondsSource monotonicSeconds)
    : recovery_(recovery), sessions_(sessions), presence_(presence),
      monotonicSeconds_(monotonicSeconds), body_(), rejected_(false),
      pendingStatus_(0), pendingError_(nullptr) {}

Esp32RecoveryHttpHandler::Route Esp32RecoveryHttpHandler::route_(
    HTTPMethod method, const String& uri) const {
  if (method == HTTP_GET) {
    if (uri == kPreviewUri) return ROUTE_PREVIEW;
    if (uri == kPendingUri) return ROUTE_PENDING;
  } else if (method == HTTP_POST) {
    if (uri == kPrepareUri) return ROUTE_PREPARE;
    if (uri == kResumeUri) return ROUTE_RESUME;
  }
  return ROUTE_NONE;
}

bool Esp32RecoveryHttpHandler::canHandle(
    WebServer& server, HTTPMethod method, const String& uri) {
  (void)server;
  return route_(method, uri) != ROUTE_NONE;
}

bool Esp32RecoveryHttpHandler::canRaw(
    WebServer& server, const String& uri) {
  return route_(server.method(), uri) == ROUTE_PREPARE ||
         route_(server.method(), uri) == ROUTE_RESUME;
}

bool Esp32RecoveryHttpHandler::canUpload(
    WebServer& server, const String& uri) {
  return canRaw(server, uri);
}

int Esp32RecoveryHttpHandler::contentLength_(
    const String& value, size_t& out) const {
  if (value.length() == 0) return 411;
  size_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return 400;
    const size_t digit = static_cast<size_t>(c - '0');
    if (digit > RECOVERY_HTTP_REQUEST_MAX_BYTES ||
        parsed > (RECOVERY_HTTP_REQUEST_MAX_BYTES - digit) / 10u)
      return 413;
    parsed = parsed * 10u + digit;
  }
  if (parsed == 0) return 400;
  out = parsed;
  return 0;
}

void Esp32RecoveryHttpHandler::wipeBody_() {
  body_.clear();
}

void Esp32RecoveryHttpHandler::reject_(int status, const char* error) {
  if (!rejected_) {
    pendingStatus_ = status;
    pendingError_ = error;
  }
  rejected_ = true;
  body_.abort();
}

void Esp32RecoveryHttpHandler::raw(
    WebServer& server, const String& uri, HTTPRaw& rawData) {
  const Route route = route_(HTTP_POST, uri);
  if (route != ROUTE_PREPARE && route != ROUTE_RESUME) return;
  if (rawData.status != RAW_START && rejected_) return;

  if (rawData.status == RAW_START) {
    wipeBody_();
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
    size_t declaredLength = 0;
    const int lengthStatus =
        contentLength_(server.header("Content-Length"), declaredLength);
    if (lengthStatus != 0) {
      reject_(lengthStatus,
              lengthStatus == 413 ? "request_body_too_large" :
              lengthStatus == 411 ? "content_length_required" :
                                    "invalid_content_length");
      return;
    }
    if (body_.begin(declaredLength) != FRSB_OK) {
      reject_(400, "invalid_content_length");
    }
    return;
  }

  if (rawData.status == RAW_WRITE) {
    if (body_.append(rawData.buf, rawData.currentSize,
                     rawData.totalSize) != FRSB_OK)
      reject_(400, "invalid_request_chunk");
    return;
  }
  if (rawData.status == RAW_END) {
    if (body_.finish() != FRSB_OK)
      reject_(400, "request_body_incomplete");
    return;
  }
  if (rawData.status == RAW_ABORTED) {
    reject_(400, "request_body_incomplete");
  }
}

void Esp32RecoveryHttpHandler::upload(
    WebServer& server, const String& uri, HTTPUpload& uploadData) {
  (void)server;
  const Route route = route_(HTTP_POST, uri);
  if ((route == ROUTE_PREPARE || route == ROUTE_RESUME) &&
      uploadData.status == UPLOAD_FILE_START)
    reject_(415, "multipart_file_upload_not_supported");
}

bool Esp32RecoveryHttpHandler::decodeBearer_(
    const String& value, uint8_t token[16]) const {
  static const char prefix[] = "Bearer ";
  if (token == nullptr || value.length() != 39) return false;
  for (size_t i = 0; i < sizeof prefix - 1u; ++i)
    if (value[i] != prefix[i]) return false;

  for (size_t i = 0; i < 16; ++i) {
    const char highChar = value[7u + i * 2u];
    const char lowChar = value[8u + i * 2u];
    const int high = highChar >= '0' && highChar <= '9'
        ? highChar - '0' : highChar >= 'a' && highChar <= 'f'
        ? highChar - 'a' + 10 : -1;
    const int low = lowChar >= '0' && lowChar <= '9'
        ? lowChar - '0' : lowChar >= 'a' && lowChar <= 'f'
        ? lowChar - 'a' + 10 : -1;
    if (high < 0 || low < 0) {
      wipe(token, 16);
      return false;
    }
    token[i] = static_cast<uint8_t>((high << 4) | low);
  }
  uint8_t aggregate = 0;
  for (size_t i = 0; i < 16; ++i) aggregate |= token[i];
  if (aggregate == 0) {
    wipe(token, 16);
    return false;
  }
  return true;
}

bool Esp32RecoveryHttpHandler::authorizePreviewRead_(
    WebServer& server, uint8_t token[16]) {
  if (!presence_.asserted()) {
    sendError_(server, 423, "presence_required");
    return false;
  }
  if (!decodeBearer_(server.header("Authorization"), token)) {
    sendError_(server, 401, "admin_session_required");
    return false;
  }
  if (monotonicSeconds_ == nullptr) {
    wipe(token, 16);
    sendError_(server, 503, "monotonic_clock_unavailable");
    return false;
  }
  const uint32_t nowS = monotonicSeconds_();
  const bool sessionMatched = sessions_.recoverySessionMatches_(token, nowS);
  const bool presenceStillAsserted = presence_.asserted();
  const ApiResult authorized = presenceStillAsserted && sessionMatched
      ? sessions_.authorizeRecoverySession_(token, nowS)
      : ApiResult{A_UNAUTH, R_NO_TOKEN, 0};
  wipe(token, 16);
  if (!presenceStillAsserted) {
    sendError_(server, 423, "presence_required");
    return false;
  }
  if (authorized.code != A_OK) {
    sendError_(server, 401, "admin_session_required");
    return false;
  }
  return true;
}

void Esp32RecoveryHttpHandler::sendError_(
    WebServer& server, int status, const char* error) {
  char payload[96] = {};
  const int length = snprintf(payload, sizeof payload,
                              "{\"error\":\"%s\"}",
                              error != nullptr ? error : "request_rejected");
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; frame-ancestors 'none'");
  if (length > 0 && static_cast<size_t>(length) < sizeof payload)
    server.send(status, "application/json", payload);
  else
    server.send(500, "application/json", "{\"error\":\"response_error\"}");
  wipe(payload, sizeof payload);
}

void Esp32RecoveryHttpHandler::sendPreview_(
    WebServer& server, const RecoveryPreview& preview) {
  char operationId[RECOVERY_OPERATION_ID_BYTES * 2u + 1u] = {};
  char manifestSha[sizeof preview.sourceManifestSha256 * 2u + 1u] = {};
  char payload[384] = {};
  encodeHex(preview.operationId, sizeof preview.operationId, operationId);
  encodeHex(preview.sourceManifestSha256,
            sizeof preview.sourceManifestSha256, manifestSha);
  int length = 0;
  if (preview.status == RECOVERY_PREVIEW_READY) {
    length = snprintf(
        payload, sizeof payload,
        "{\"status\":\"%s\",\"operation_id\":\"%s\","
        "\"source_count\":%lu,\"source_total_bytes\":%llu,"
        "\"retained_bytes\":%llu,\"source_manifest_sha256\":\"%s\","
        "\"plan_crc32\":%lu}",
        recoveryStatusName(preview.status), operationId,
        static_cast<unsigned long>(preview.sourceCount),
        static_cast<unsigned long long>(preview.sourceTotalBytes),
        static_cast<unsigned long long>(preview.retainedBytes), manifestSha,
        static_cast<unsigned long>(preview.planCrc32));
  } else {
    length = snprintf(payload, sizeof payload,
                      "{\"status\":\"%s\"}",
                      recoveryStatusName(preview.status));
  }

  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; frame-ancestors 'none'");
  const int status = recoveryHttpStatus(preview.status);
  if (length > 0 && static_cast<size_t>(length) < sizeof payload)
    server.send(status, "application/json", payload);
  else
    server.send(500, "application/json", "{\"error\":\"response_error\"}");
  wipe(operationId, sizeof operationId);
  wipe(manifestSha, sizeof manifestSha);
  wipe(payload, sizeof payload);
}

void Esp32RecoveryHttpHandler::sendResult_(
    WebServer& server, const RecoveryResult& result) {
  char operationId[RECOVERY_OPERATION_ID_BYTES * 2u + 1u] = {};
  char payload[256] = {};
  encodeHex(result.operationId, sizeof result.operationId, operationId);
  const int length = snprintf(
      payload, sizeof payload,
      "{\"status\":\"%s\",\"operation_id\":\"%s\","
      "\"retained_bytes\":%llu,\"files_archived\":%lu,"
      "\"files_changed\":%lu}",
      recoveryStatusName(result.status), operationId,
      static_cast<unsigned long long>(result.retainedBytes),
      static_cast<unsigned long>(result.filesArchived),
      static_cast<unsigned long>(result.filesChanged));
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; frame-ancestors 'none'");
  if (length > 0 && static_cast<size_t>(length) < sizeof payload)
    server.send(recoveryHttpStatus(result.status), "application/json", payload);
  else
    server.send(500, "application/json", "{\"error\":\"response_error\"}");
  wipe(operationId, sizeof operationId);
  wipe(payload, sizeof payload);
}

void Esp32RecoveryHttpHandler::dispatch_(WebServer& server, Route route) {
  if (!presence_.asserted()) {
    sendError_(server, 423, "presence_required");
    return;
  }
  if (route == ROUTE_PREVIEW || route == ROUTE_PENDING) {
    if (server.args() != 0) {
      sendError_(server, 400, "query_parameters_not_supported");
      return;
    }
    uint8_t token[16] = {};
    if (!authorizePreviewRead_(server, token)) return;
    const RecoveryPreview preview = route == ROUTE_PREVIEW
        ? recovery_.preview() : recovery_.pendingPreview();
    if (!presence_.asserted()) {
      sendError_(server, 423, "presence_required");
      return;
    }
    sendPreview_(server, preview);
    return;
  }

  if (route != ROUTE_PREPARE && route != ROUTE_RESUME) return;
  if (server.args() != 0) {
    sendError_(server, 400, "query_parameters_not_supported");
    wipeBody_();
    return;
  }
  if (rejected_) {
    sendError_(server, pendingStatus_ != 0 ? pendingStatus_ : 400,
               pendingError_ != nullptr ? pendingError_ : "request_rejected");
    wipeBody_();
    return;
  }
  if (!body_.ready()) {
    sendError_(server, 400, "request_body_unavailable");
    wipeBody_();
    return;
  }

  RecoveryHttpRequest request = {};
  if (!decodeRecoveryHttpRequest(body_.data(), body_.length(), request)) {
    wipeBody_();
    clearRecoveryHttpRequest(request);
    sendError_(server, 422, "invalid_recovery_request");
    return;
  }
  wipeBody_();

  uint8_t token[16] = {};
  if (!decodeBearer_(server.header("Authorization"), token)) {
    clearRecoveryHttpRequest(request);
    sendError_(server, 401, "admin_session_required");
    return;
  }
  if (monotonicSeconds_ == nullptr) {
    wipe(token, sizeof token);
    clearRecoveryHttpRequest(request);
    sendError_(server, 503, "monotonic_clock_unavailable");
    return;
  }

  AdminRecoveryAuthorization authorization(
      sessions_, presence_, token, monotonicSeconds_(),
      request.confirmation, strlen(request.confirmation));
  wipe(token, sizeof token);
  RecoveryResult result = route == ROUTE_PREPARE
      ? recovery_.prepare(request.preview, authorization)
      : recovery_.resumePending(request.preview, authorization);
  clearRecoveryHttpRequest(request);
  if (result.status == RECOVERY_COMPLETE && !recovery_.end())
    result.status = RECOVERY_BACKEND_FAILURE;
  sendResult_(server, result);
}

bool Esp32RecoveryHttpHandler::handle(
    WebServer& server, HTTPMethod method, const String& uri) {
  const Route route = route_(method, uri);
  if (route == ROUTE_NONE) return false;
  if (route == ROUTE_PREPARE || route == ROUTE_RESUME) {
    if (rejected_) {
      sendError_(server, pendingStatus_ != 0 ? pendingStatus_ : 400,
                 pendingError_ != nullptr ? pendingError_ : "request_rejected");
      wipeBody_();
      return true;
    }
    if (!body_.ready()) {
      sendError_(server, 400, "request_body_unavailable");
      wipeBody_();
      return true;
    }
  }
  dispatch_(server, route);
  return true;
}

}  // namespace att

#endif  // ESP_PLATFORM
