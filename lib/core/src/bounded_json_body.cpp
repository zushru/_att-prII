#include "bounded_json_body.h"

namespace att {

BoundedJsonBody::BoundedJsonBody()
    : body_{0}, expected_(0), received_(0), state_(JBS_EMPTY),
      httpStatus_(0), error_(nullptr) {}

BoundedJsonBody::~BoundedJsonBody() {
  wipe_(body_, sizeof body_);
}

void BoundedJsonBody::wipe_(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- != 0) *bytes++ = 0;
}

bool BoundedJsonBody::isApplicationJson_(const char* value) {
  static const char expected[] = "application/json";
  if (value == nullptr) return false;
  size_t i = 0;
  for (; expected[i] != '\0' && value[i] != '\0'; ++i) {
    char actual = value[i];
    char wanted = expected[i];
    if (actual >= 'A' && actual <= 'Z')
      actual = static_cast<char>(actual - 'A' + 'a');
    if (actual != wanted) return false;
  }
  return expected[i] == '\0' && value[i] == '\0';
}

int BoundedJsonBody::parseLength_(const char* value, size_t& out) {
  out = 0;
  if (value == nullptr || value[0] == '\0') return 411;
  size_t parsed = 0;
  size_t digits = 0;
  for (size_t i = 0; value[i] != '\0'; ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return 400;
    if (++digits > 3) return 413;
    const size_t digit = static_cast<size_t>(c - '0');
    if (parsed > (BOUNDED_JSON_BODY_MAX_BYTES - digit) / 10u)
      return 413;
    parsed = parsed * 10u + digit;
  }
  if (digits > 1 && value[0] == '0') return 400;
  if (parsed == 0) return 400;
  if (parsed > BOUNDED_JSON_BODY_MAX_BYTES) return 413;
  out = parsed;
  return 0;
}

void BoundedJsonBody::reject_(int httpStatus, const char* error) {
  wipe_(body_, sizeof body_);
  expected_ = 0;
  received_ = 0;
  state_ = JBS_REJECTED;
  httpStatus_ = httpStatus;
  error_ = error != nullptr ? error : "invalid_request_body";
}

bool BoundedJsonBody::begin(const char* contentType,
                            const char* contentLength,
                            int clientContentLength,
                            bool transferEncodingPresent) {
  // RAW_ABORTED is terminal: the framework may return without dispatching
  // handle(), so the next request start must recover this per-handler buffer.
  // All other non-empty states require the adapter's explicit post-dispatch
  // reset; accepting another start there could replace a live request.
  if (state_ == JBS_ABORTED) reset();
  if (state_ != JBS_EMPTY) {
    if (state_ != JBS_REJECTED)
      reject_(400, "unexpected_request_body_start");
    return false;
  }
  if (transferEncodingPresent) {
    reject_(400, "transfer_encoding_not_supported");
    return false;
  }
  if (!isApplicationJson_(contentType)) {
    reject_(415, "application_json_required");
    return false;
  }
  const int lengthStatus = parseLength_(contentLength, expected_);
  if (lengthStatus != 0) {
    reject_(lengthStatus,
        lengthStatus == 411 ? "content_length_required" :
        lengthStatus == 413 ? "request_body_too_large" :
                              "invalid_content_length");
    return false;
  }
  if (clientContentLength < 0 ||
      static_cast<size_t>(clientContentLength) != expected_) {
    reject_(400, "content_length_mismatch");
    return false;
  }
  state_ = JBS_RECEIVING;
  return true;
}

bool BoundedJsonBody::append(size_t totalSize, size_t chunkSize,
                             const uint8_t* chunk) {
  if (rejected()) return false;
  if (state_ != JBS_RECEIVING || chunk == nullptr || chunkSize == 0 ||
      received_ > expected_ || chunkSize > expected_ - received_ ||
      totalSize != received_ + chunkSize) {
    reject_(400, "invalid_request_chunk");
    return false;
  }
  for (size_t i = 0; i < chunkSize; ++i)
    body_[received_ + i] = static_cast<char>(chunk[i]);
  received_ += chunkSize;
  return true;
}

bool BoundedJsonBody::finish() {
  if (rejected()) return false;
  if (state_ != JBS_RECEIVING || received_ != expected_) {
    reject_(400, "request_body_incomplete");
    return false;
  }
  body_[received_] = '\0';
  state_ = JBS_READY;
  httpStatus_ = 0;
  error_ = nullptr;
  return true;
}

void BoundedJsonBody::abort() {
  reject(400, "request_body_incomplete");
  state_ = JBS_ABORTED;
}

void BoundedJsonBody::reject(int httpStatus, const char* error) {
  if (!rejected()) reject_(httpStatus, error);
}

void BoundedJsonBody::reset() {
  wipe_(body_, sizeof body_);
  expected_ = 0;
  received_ = 0;
  state_ = JBS_EMPTY;
  httpStatus_ = 0;
  error_ = nullptr;
}

}  // namespace att
