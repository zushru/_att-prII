#pragma once

#include <stddef.h>
#include <stdint.h>

#include <WebServer.h>

#include "clock_confirmation_request.h"
#include "clock_health_owner.h"

namespace att {

class Esp32AdminClockHttpHandler : public RequestHandler {
 public:
  typedef uint32_t (*MonotonicSecondsSource)();
  typedef bool (*CurrentHealthSource)(HealthInputs& out);

  Esp32AdminClockHttpHandler(ClockHealthOwner& clockOwner, LogStore& log,
                             MonthSet& months,
                             MonotonicSecondsSource monotonicSeconds,
                             CurrentHealthSource currentHealth,
                             bool& applicationFaulted);
  Esp32AdminClockHttpHandler(const Esp32AdminClockHttpHandler&) = delete;
  Esp32AdminClockHttpHandler& operator=(
      const Esp32AdminClockHttpHandler&) = delete;

  bool canHandle(WebServer& server, HTTPMethod method,
                 const String& uri) override;
  bool canRaw(WebServer& server, const String& uri) override;
  bool canUpload(WebServer& server, const String& uri) override;
  bool handle(WebServer& server, HTTPMethod method,
              const String& uri) override;
  void raw(WebServer& server, const String& uri, HTTPRaw& raw) override;
  void upload(WebServer& server, const String& uri,
              HTTPUpload& upload) override;

 private:
  bool contentLength_(const String& value, size_t& out) const;
  void clearBody_();
  void reject_(int status, const char* error);
  void sendError_(WebServer& server, int status, const char* error);
  void sendJson_(WebServer& server, int status, const char* json);
  void dispatch_(WebServer& server);

  ClockHealthOwner& clockOwner_;
  LogStore& log_;
  MonthSet& months_;
  MonotonicSecondsSource monotonicSeconds_;
  CurrentHealthSource currentHealth_;
  bool& applicationFaulted_;
  char body_[CLOCK_CONFIRM_REQUEST_MAX_BYTES + 1];
  size_t expectedLength_;
  size_t receivedLength_;
  bool receiving_;
  bool bodyReady_;
  bool rejected_;
  int pendingStatus_;
  const char* pendingError_;
};

}  // namespace att
