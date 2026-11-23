// HTTP adapters are production-only; native tests exercise their core boundaries.
#if defined(ESP_PLATFORM)
#include "esp32_admin_auth_http_handler.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>

namespace att {
namespace {

const char kLoginUri[] = "/api/v1/auth/login";
const char kLogoutUri[] = "/api/v1/auth/logout";
const char kPinChangeUri[] = "/api/v1/auth/pin";
// Bounded to 250 ms to prevent client stalls from starving the main loop
// and overflowing the 355 ms AS608 UART buffer.
const uint32_t kResponseWriteTimeoutMs = 250;

void wipe(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 429: return "Too Many Requests";
    default: return "Service Unavailable";
  }
}



}  // namespace

Esp32AdminAuthHttpHandler::Esp32AdminAuthHttpHandler(
    AdminSessionBoundary& sessions, HealthFsm& health,
    IAdminPinWriter& pinWriter, MonotonicSecondsSource monotonicSeconds,
    bool& applicationFaulted)
    : sessions_(sessions), health_(health),
      pinWriter_(pinWriter), monotonicSeconds_(monotonicSeconds),
      applicationFaulted_(applicationFaulted), body_(), rejected_(false),
      pendingStatus_(0), pendingError_(nullptr) {}

bool Esp32AdminAuthHttpHandler::route_(
    const String& uri, AdminAuthRequestKind& kind) const {
  if (uri == kLoginUri) {
    kind = AAR_LOGIN;
    return true;
  }
  if (uri == kLogoutUri) {
    kind = AAR_LOGOUT;
    return true;
  }
  if (uri == kPinChangeUri) {
    kind = AAR_PIN_CHANGE;
    return true;
  }
  return false;
}

bool Esp32AdminAuthHttpHandler::canHandle(
    WebServer& server, HTTPMethod method, const String& uri) {
  (void)server;
  AdminAuthRequestKind kind = AAR_LOGIN;
  return method == HTTP_POST && route_(uri, kind);
}

bool Esp32AdminAuthHttpHandler::canRaw(
    WebServer& server, const String& uri) {
  AdminAuthRequestKind kind = AAR_LOGIN;
  return server.method() == HTTP_POST && route_(uri, kind);
}

bool Esp32AdminAuthHttpHandler::canUpload(
    WebServer& server, const String& uri) {
  AdminAuthRequestKind kind = AAR_LOGIN;
  return server.method() == HTTP_POST && route_(uri, kind);
}

int Esp32AdminAuthHttpHandler::contentLength_(
    const String& value, size_t& out) const {
  if (value.length() == 0) return 411;
  size_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return 400;
    const size_t digit = static_cast<size_t>(c - '0');
    if (digit > ADMIN_AUTH_REQUEST_MAX_BYTES ||
        parsed > (ADMIN_AUTH_REQUEST_MAX_BYTES - digit) / 10u)
      return 413;
    parsed = parsed * 10u + digit;
  }
  if (parsed == 0) return 400;
  out = parsed;
  return 0;
}

void Esp32AdminAuthHttpHandler::wipeBody_() {
  body_.clear();
}

void Esp32AdminAuthHttpHandler::reject_(int status, const char* error) {
  if (!rejected_) {
    pendingStatus_ = status;
    pendingError_ = error;
  }
  rejected_ = true;
  wipeBody_();
}

void Esp32AdminAuthHttpHandler::raw(
    WebServer& server, const String& uri, HTTPRaw& rawData) {
  AdminAuthRequestKind kind = AAR_LOGIN;
  if (!route_(uri, kind)) return;
  if (rawData.status != RAW_START && rejected_) return;

  if (rawData.status == RAW_START) {
    wipeBody_();
    rejected_ = false;
    pendingStatus_ = 0;
    pendingError_ = nullptr;

    const String transferEncoding = server.header("Transfer-Encoding");
    const String contentType = server.header("Content-Type");
    size_t declaredLength = 0;
    if (transferEncoding.length() != 0) {
      reject_(400, "transfer_encoding_not_supported");
      return;
    }
    if (!contentType.equalsIgnoreCase("application/json")) {
      reject_(415, "application_json_required");
      return;
    }
    const int lengthStatus =
        contentLength_(server.header("Content-Length"), declaredLength);
    if (lengthStatus != 0) {
      reject_(lengthStatus,
              lengthStatus == 413 ? "request_body_too_large" :
              lengthStatus == 411 ? "content_length_required" :
                                    "invalid_content_length");
      return;
    }
    if (server.clientContentLength() != static_cast<int>(declaredLength)) {
      reject_(400, "content_length_mismatch");
      return;
    }
    const FirstRunSetupBodyCode beginCode = body_.begin(declaredLength);
    if (beginCode != FRSB_OK) {
      reject_(beginCode == FRSB_TOO_LARGE ? 413 : 400,
              beginCode == FRSB_TOO_LARGE
                  ? "request_body_too_large" : "invalid_content_length");
    }
    return;
  }

  if (rawData.status == RAW_WRITE) {
    const FirstRunSetupBodyCode appendCode = body_.append(
        rawData.buf, rawData.currentSize, rawData.totalSize);
    if (appendCode != FRSB_OK) {
      reject_(appendCode == FRSB_TOO_LARGE ? 413 : 400,
              appendCode == FRSB_TOO_LARGE
                  ? "request_body_too_large" : "invalid_request_chunk");
    }
    return;
  }

  if (rawData.status == RAW_END) {
    if (body_.finish() != FRSB_OK)
      reject_(400, "request_body_incomplete");
    return;
  }

  if (rawData.status == RAW_ABORTED) {
    body_.abort();
    reject_(400, "request_body_incomplete");
  }
}

void Esp32AdminAuthHttpHandler::upload(
    WebServer& server, const String& uri, HTTPUpload& uploadData) {
  (void)server;
  AdminAuthRequestKind kind = AAR_LOGIN;
  if (route_(uri, kind) && uploadData.status == UPLOAD_FILE_START)
    reject_(415, "multipart_file_upload_not_supported");
}

void Esp32AdminAuthHttpHandler::sendReply_(
    WebServer& server, int status, const char* error, uint32_t retryAfterS) {
  char payload[96] = {};
  const char* safeError = error != nullptr ? error : "request_rejected";
  const int length = snprintf(payload, sizeof payload,
                              "{\"error\":\"%s\"}", safeError);
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; frame-ancestors 'none'");
  if (retryAfterS != 0) {
    char retry[16] = {};
    (void)snprintf(retry, sizeof retry, "%lu",
                   static_cast<unsigned long>(retryAfterS));
    server.sendHeader("Retry-After", retry);
  }
  if (length > 0 && static_cast<size_t>(length) < sizeof payload)
    server.send(status, "application/json", payload);
  else
    server.send(500, "application/json", "{\"error\":\"response_error\"}");
  wipe(payload, sizeof payload);
}

bool Esp32AdminAuthHttpHandler::writeAll_(
    NetworkClient& client, const uint8_t* bytes, size_t length,
    uint32_t started) {
  if ((bytes == nullptr && length != 0) ||
      static_cast<uint32_t>(millis() - started) >=
          kResponseWriteTimeoutMs)
    return false;
  const size_t kMaxResponseChunkBytes = 64;
  size_t offset = 0;
  while (offset < length) {
    if (!client.connected() ||
        static_cast<uint32_t>(millis() - started) >=
            kResponseWriteTimeoutMs)
      return false;

    const size_t remaining = length - offset;
    const size_t chunk = remaining < kMaxResponseChunkBytes
        ? remaining : kMaxResponseChunkBytes;
    const size_t written = client.write(bytes + offset, chunk);
    if (written > chunk) return false;
    if (written == 0) {
      if (static_cast<uint32_t>(millis() - started) >=
          kResponseWriteTimeoutMs)
        return false;
      delay(1);
      continue;
    }

    offset += written;
    // A successful final write is still a timeout if the socket call blocked
    // past the whole-response deadline.
    if (static_cast<uint32_t>(millis() - started) >=
        kResponseWriteTimeoutMs)
      return false;
  }
  return static_cast<uint32_t>(millis() - started) <
         kResponseWriteTimeoutMs;
}

bool Esp32AdminAuthHttpHandler::writeResponse_(
    WebServer& server, int status, const char* body, size_t bodyLength) {
  if ((body == nullptr && bodyLength != 0) ||
      (status != 200 && status != 204) ||
      (status == 204 && bodyLength != 0))
    return false;

  char representationHeaders[80] = {};
  if (status != 204) {
    const int representationLength = snprintf(
        representationHeaders, sizeof representationHeaders,
        "Content-Type: application/json\r\n"
        "Content-Length: %lu\r\n",
        static_cast<unsigned long>(bodyLength));
    if (representationLength <= 0 ||
        static_cast<size_t>(representationLength) >=
            sizeof representationHeaders) {
      wipe(representationHeaders, sizeof representationHeaders);
      return false;
    }
  }

  char header[320] = {};
  const int headerLength = snprintf(
      header, sizeof header,
      "HTTP/1.1 %d %s\r\n"
      "%s"
      "Cache-Control: no-store, max-age=0\r\n"
      "Pragma: no-cache\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Referrer-Policy: no-referrer\r\n"
      "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
      "Connection: close\r\n\r\n",
      status, statusText(status), representationHeaders);
  wipe(representationHeaders, sizeof representationHeaders);
  if (headerLength <= 0 || static_cast<size_t>(headerLength) >= sizeof header) {
    wipe(header, sizeof header);
    return false;
  }

  NetworkClient& client = server.client();
  const uint32_t responseStarted = millis();
  const bool headerWritten = writeAll_(
      client, reinterpret_cast<const uint8_t*>(header),
      static_cast<size_t>(headerLength), responseStarted);
  const bool bodyWritten = headerWritten && writeAll_(
      client, reinterpret_cast<const uint8_t*>(body), bodyLength,
      responseStarted);
  wipe(header, sizeof header);
  return bodyWritten;
}

void Esp32AdminAuthHttpHandler::dispatch_(
    WebServer& server, AdminAuthRequestKind kind) {
  if (monotonicSeconds_ == nullptr) {
    sendReply_(server, 503, "monotonic_clock_unavailable");
    return;
  }

  AdminAuthRequest request = {};
  const bool decoded = decodeAdminAuthRequest(
      kind, body_.data(), body_.length(), request);
  wipeBody_();
  if (!decoded) {
    clearAdminAuthRequest(request);
    sendReply_(server, 422, "invalid_auth_request");
    return;
  }

  const uint32_t nowS = monotonicSeconds_();
  if (kind == AAR_LOGIN) {
    const ApiResult health = att::authorize(
        EP_LOGIN, health_.state(), false, false);
    if (health.code != A_OK) {
      sendReply_(server, health.code == A_BAD ? 400 : 423,
                 health.reason == R_BAD_FIELD
                     ? "invalid_health_state" : "login_health_blocked");
      clearAdminAuthRequest(request);
      return;
    }

    uint8_t token[16] = {};
    const PinAuthResult login =
        sessions_.beginLogin(request.pin, 6, nowS, token);
    if (login.code != PAC_OK) {
      sendReply_(server,
                 login.code == PAC_BAD_SECRET ? 401 :
                 login.code == PAC_LOCKED ? 429 :
                 login.code == PAC_BAD_INPUT ? 422 :
                 login.code == PAC_ACTION_REFUSED ? 409 :
                 login.code == PAC_NOT_READY ? 423 : 503,
                 login.code == PAC_BAD_SECRET ? "invalid_pin" :
                 login.code == PAC_LOCKED ? "auth_locked" :
                 login.code == PAC_BAD_INPUT ? "invalid_pin" :
                 login.code == PAC_ACTION_REFUSED ? "login_refused" :
                 login.code == PAC_NOT_READY ? "auth_unavailable" :
                                                "auth_fault",
                 login.retryAfterS);
      if (sessions_.faulted()) applicationFaulted_ = true;
      wipe(token, sizeof token);
      clearAdminAuthRequest(request);
      return;
    }

    static const char hex[] = "0123456789abcdef";
    char tokenHex[33] = {};
    for (size_t i = 0; i < sizeof token; ++i) {
      tokenHex[i * 2] = hex[token[i] >> 4];
      tokenHex[i * 2 + 1] = hex[token[i] & 0x0f];
    }
    char payload[96] = {};
    const int payloadLength = snprintf(
        payload, sizeof payload,
        "{\"token\":\"%s\",\"expires_in\":%lu}",
        tokenHex, static_cast<unsigned long>(TOKEN_IDLE_S));
    const bool prepared = payloadLength > 0 &&
        static_cast<size_t>(payloadLength) < sizeof payload;
    const bool written = prepared && writeResponse_(
        server, 200, payload, static_cast<size_t>(payloadLength));
    if (!written) {
      sessions_.abortLoginResponse();
      server.client().stop();
      wipe(token, sizeof token);
      wipe(tokenHex, sizeof tokenHex);
      wipe(payload, sizeof payload);
      clearAdminAuthRequest(request);
      return;
    }

    // The session becomes live only after the complete fixed-length response
    // has been accepted by the current socket writer.
    if (!sessions_.confirmLoginResponseSent(monotonicSeconds_())) {
      applicationFaulted_ = true;
      server.client().stop();
    }
    wipe(token, sizeof token);
    wipe(tokenHex, sizeof tokenHex);
    wipe(payload, sizeof payload);
    clearAdminAuthRequest(request);
    return;
  }

  if (kind == AAR_LOGOUT) {
    const ApiResult authorization = sessions_.authorize(
        EP_LOGOUT, health_.state(), request.token, nowS);
    if (authorization.code != A_OK) {
      sendReply_(server,
                 authorization.code == A_UNAUTH ? 401 :
                 authorization.code == A_LOCKED ? 423 : 400,
                 authorization.code == A_UNAUTH ? "unauthorized" :
                 authorization.code == A_LOCKED ? "health_blocked" :
                                                   "invalid_request");
    } else {
      if (!writeResponse_(server, 204, "", 0)) server.client().stop();
    }
    if (sessions_.faulted()) applicationFaulted_ = true;
    clearAdminAuthRequest(request);
    return;
  }

  if (kind == AAR_PIN_CHANGE) {
    const AdminPinChangeDispatchResult changed = sessions_.changeAdminPin(
        request.token, health_.state(), request.currentPin, 6,
        request.newPin, 6, nowS, pinWriter_);
    if (changed.code == APCD_CHANGE_COMMITTED) {
      if (!writeResponse_(server, 204, "", 0)) server.client().stop();
    } else if (changed.code == APCD_AUTHORIZATION_DENIED) {
      sendReply_(server,
                 changed.authorization.code == A_UNAUTH ? 401 :
                 changed.authorization.code == A_LOCKED ? 423 : 400,
                 changed.authorization.code == A_UNAUTH ? "unauthorized" :
                 changed.authorization.code == A_LOCKED ? "health_blocked" :
                                                            "invalid_request");
    } else {
      const PinAuthResult& result = changed.pinChange;
      sendReply_(server,
                 result.code == PAC_BAD_SECRET ? 401 :
                 result.code == PAC_LOCKED ? 429 :
                 result.code == PAC_BAD_INPUT ? 422 :
                 result.code == PAC_WEAK_NEW_PIN ? 422 :
                 result.code == PAC_ACTION_REFUSED ? 409 :
                 result.code == PAC_NOT_READY ? 423 : 503,
                 result.code == PAC_BAD_SECRET ? "current_pin_invalid" :
                 result.code == PAC_LOCKED ? "auth_locked" :
                 result.code == PAC_BAD_INPUT ? "invalid_pin" :
                 result.code == PAC_WEAK_NEW_PIN ? "weak_new_pin" :
                 result.code == PAC_ACTION_REFUSED ? "pin_change_refused" :
                 result.code == PAC_NOT_READY ? "auth_unavailable" :
                                                 "pin_change_fault",
                 result.retryAfterS);
    }
    if (sessions_.faulted()) applicationFaulted_ = true;
    clearAdminAuthRequest(request);
    return;
  }


  clearAdminAuthRequest(request);
  sendReply_(server, 400, "invalid_auth_operation");
}

bool Esp32AdminAuthHttpHandler::handle(
    WebServer& server, HTTPMethod method, const String& uri) {
  AdminAuthRequestKind kind = AAR_LOGIN;
  if (method != HTTP_POST || !route_(uri, kind)) return false;
  if (rejected_) {
    sendReply_(server, pendingStatus_ != 0 ? pendingStatus_ : 400,
               pendingError_ != nullptr ? pendingError_ : "request_rejected");
    wipeBody_();
    return true;
  }
  if (!body_.ready()) {
    sendReply_(server, 400, "request_body_unavailable");
    wipeBody_();
    return true;
  }
  dispatch_(server, kind);
  return true;
}

}  // namespace att
#endif  // ESP_PLATFORM
