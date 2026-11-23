#pragma once

#include <stddef.h>
#include <stdint.h>

#include <WebServer.h>

#include "admin_auth_request.h"
#include "admin_session.h"
#include "esp32_mbedtls_pin_writer.h"
#include "first_run_setup_request.h"
#include "health_fsm.h"

namespace att {

class Esp32AdminAuthHttpHandler : public RequestHandler {
 public:
  typedef uint32_t (*MonotonicSecondsSource)();

  Esp32AdminAuthHttpHandler(
      AdminSessionBoundary& sessions,
      HealthFsm& health,
      IAdminPinWriter& pinWriter,
      MonotonicSecondsSource monotonicSeconds,
      bool& applicationFaulted);

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
  bool route_(const String& uri, AdminAuthRequestKind& kind) const;
  int contentLength_(const String& value, size_t& out) const;
  void wipeBody_();
  void reject_(int status, const char* error);
  void sendReply_(WebServer& server, int status, const char* error,
                  uint32_t retryAfterS = 0);
  bool writeResponse_(WebServer& server, int status,
                      const char* body, size_t bodyLength);
  bool writeAll_(NetworkClient& client, const uint8_t* bytes, size_t length, uint32_t started);
  void dispatch_(WebServer& server, AdminAuthRequestKind kind);

  AdminSessionBoundary& sessions_;
  HealthFsm& health_;
  IAdminPinWriter& pinWriter_;
  MonotonicSecondsSource monotonicSeconds_;
  bool& applicationFaulted_;
  FirstRunSetupBodyBuffer body_;
  bool rejected_;
  int pendingStatus_;
  const char* pendingError_;
};

}  // namespace att
