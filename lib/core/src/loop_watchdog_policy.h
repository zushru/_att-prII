#pragma once

#include <stdint.h>

namespace att {

// A single synchronous WebServer::handleClient() call can wait for request
// headers, a raw request body, and TCP response writes. Arduino-ESP32
// WebServer 3.3.12 defaults are:
//   WEBSERVER_MAX_HEADER_WAIT = 10000 ms
//   HTTP_MAX_POST_WAIT        =  5000 ms
//   HTTP_MAX_SEND_WAIT        =  5000 ms
// The ATT parser patch preserves the bounded 5000 ms body deadline. Normal
// non-SSE responses close immediately; HTTP_MAX_CLOSE_WAIT is not spent in
// the same handleClient() call. Keep these values synchronized with the
// pinned framework and parser patch. A future framework change must update
// this budget and its regression tests before it is accepted.
constexpr uint32_t kWebServerHeaderWaitMs = 10000;
constexpr uint32_t kWebServerBodyWaitMs = 5000;
constexpr uint32_t kWebServerSendWaitMs = 5000;
constexpr uint32_t kWebServerWorstCaseBlockMs =
    kWebServerHeaderWaitMs + kWebServerBodyWaitMs + kWebServerSendWaitMs;

// Leave ten seconds for handler overhead, scheduler jitter, response framing
// and timer granularity. The ESP32 task watchdog must be strictly longer than
// every bounded synchronous server wait, not equal to the sum of defaults.
constexpr uint32_t kLoopWatchdogMarginMs = 10000;
constexpr uint32_t kLoopWatchdogTimeoutMs = 30000;

static_assert(kLoopWatchdogTimeoutMs > kWebServerWorstCaseBlockMs,
              "loop watchdog must outlast one bounded WebServer call");
static_assert(kLoopWatchdogTimeoutMs >=
                  kWebServerWorstCaseBlockMs + kLoopWatchdogMarginMs,
              "loop watchdog must preserve its explicit margin");
static_assert(kLoopWatchdogTimeoutMs <= 30000,
              "loop watchdog must still detect a genuine hang promptly");

}  // namespace att
