#pragma once

#if defined(ESP_PLATFORM)

#include <stddef.h>
#include <stdint.h>

#include <WebServer.h>

#include "admin_recovery_authorization.h"
#include "esp32_recovery_service.h"
#include "first_run_setup_request.h"

namespace att {

// Recovery-only HTTP surface. It deliberately has no reference to LogStore,
// RosterStore, ClosureEngine, or attendance admission projections.
class Esp32RecoveryHttpHandler final : public RequestHandler {
 public:
  typedef uint32_t (*MonotonicSecondsSource)();

  Esp32RecoveryHttpHandler(Esp32RecoveryService& recovery,
                          AdminSessionBoundary& sessions,
                          IRecoveryPhysicalPresence& presence,
                          MonotonicSecondsSource monotonicSeconds);

  Esp32RecoveryHttpHandler(const Esp32RecoveryHttpHandler&) = delete;
  Esp32RecoveryHttpHandler& operator=(const Esp32RecoveryHttpHandler&) = delete;

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
  enum Route : uint8_t {
    ROUTE_NONE,
    ROUTE_PREVIEW,
    ROUTE_PENDING,
    ROUTE_PREPARE,
    ROUTE_RESUME
  };

  Route route_(HTTPMethod method, const String& uri) const;
  int contentLength_(const String& value, size_t& out) const;
  void reject_(int status, const char* error);
  void sendError_(WebServer& server, int status, const char* error);
  void sendPreview_(WebServer& server, const RecoveryPreview& preview);
  void sendResult_(WebServer& server, const RecoveryResult& result);
  bool authorizePreviewRead_(WebServer& server, uint8_t token[16]);
  bool decodeBearer_(const String& value, uint8_t token[16]) const;
  void dispatch_(WebServer& server, Route route);
  void wipeBody_();

  Esp32RecoveryService& recovery_;
  AdminSessionBoundary& sessions_;
  IRecoveryPhysicalPresence& presence_;
  MonotonicSecondsSource monotonicSeconds_;
  FirstRunSetupBodyBuffer body_;
  bool rejected_;
  int pendingStatus_;
  const char* pendingError_;
};

}  // namespace att

#endif  // ESP_PLATFORM
