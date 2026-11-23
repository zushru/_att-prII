#pragma once

#include <stddef.h>
#include <stdint.h>

namespace att {

// Shared transport limit for attendance and roster mutation decoders. The
// ESP32 adapter and host regression suite both assert this stays aligned.
static const size_t BOUNDED_JSON_BODY_MAX_BYTES = 256;

enum JsonBodyState : uint8_t {
  JBS_EMPTY,
  JBS_RECEIVING,
  JBS_READY,
  JBS_REJECTED,
  JBS_ABORTED
};

// Error values intentionally carry the HTTP response status and a fixed,
// non-sensitive error token. The parser owns the body buffer and wipes it on
// every rejection/reset. No heap allocation is performed.
class BoundedJsonBody {
 public:
  BoundedJsonBody();
  ~BoundedJsonBody();
  BoundedJsonBody(const BoundedJsonBody&) = delete;
  BoundedJsonBody& operator=(const BoundedJsonBody&) = delete;

  // Starts one application/json request. The WebServer adapter remains
  // responsible for rejecting duplicate framing headers before RAW_START.
  bool begin(const char* contentType, const char* contentLength,
             int clientContentLength, bool transferEncodingPresent);
  // totalSize is the framework's cumulative byte count after this chunk.
  bool append(size_t totalSize, size_t chunkSize, const uint8_t* chunk);
  bool finish();
  void abort();
  void reject(int httpStatus, const char* error);
  void reset();

  JsonBodyState state() const { return state_; }
  bool ready() const { return state_ == JBS_READY; }
  bool rejected() const {
    return state_ == JBS_REJECTED || state_ == JBS_ABORTED;
  }
  int httpStatus() const { return httpStatus_; }
  const char* error() const { return error_; }
  const char* data() const { return ready() ? body_ : nullptr; }
  size_t length() const { return ready() ? received_ : 0; }

 private:
  void reject_(int httpStatus, const char* error);
  static bool isApplicationJson_(const char* value);
  static int parseLength_(const char* value, size_t& out);
  static void wipe_(void* value, size_t length);

  char body_[BOUNDED_JSON_BODY_MAX_BYTES + 1];
  size_t expected_;
  size_t received_;
  JsonBodyState state_;
  int httpStatus_;
  const char* error_;
};

}  // namespace att
