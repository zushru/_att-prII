#pragma once
#include <stddef.h>
#include <stdint.h>
#include <WebServer.h>
#include "admin_session.h"
#include "day_view.h"
#include "log_store.h"
#include "month_set.h"
#include "config_store.h"
#include "health_fsm.h"
#include "roster_store.h"
#include "roster_event_writer.h"
#include "roster_mutation_request.h"
#include "attendance_mutation_request.h"
#include "bounded_json_body.h"
#include "attendance_event_writer.h"
#include "ds3231_clock.h"
#include "clock_health_owner.h"
#include "log_index.h"
namespace att {
class Esp32AdminRosterHttpHandler : public RequestHandler {
 public:
  typedef uint32_t (*MonotonicSecondsSource)();
  Esp32AdminRosterHttpHandler(AdminSessionBoundary& sessions,
      LogStore& log, MonthSet& monthData, RosterStore& roster,
      const Config& config, HealthFsm& health,
      MonotonicSecondsSource monotonicSeconds, bool& applicationFaulted,
      RosterEventWriter* writer = nullptr, Ds3231Clock* rtcClock = nullptr,
      ClockHealthOwner* clockHealth = nullptr,
      bool* storageWritesAuthorized = nullptr,
      AttendanceEventWriter* attendanceWriter = nullptr,
      const LogIndex* logIndex = nullptr);
  Esp32AdminRosterHttpHandler(const Esp32AdminRosterHttpHandler&) = delete;
  Esp32AdminRosterHttpHandler& operator=(const Esp32AdminRosterHttpHandler&) = delete;
  bool canHandle(WebServer& server, HTTPMethod method, const String& uri) override;
  bool canRaw(WebServer& server, const String& uri) override;
  bool canUpload(WebServer& server, const String& uri) override;
  bool handle(WebServer& server, HTTPMethod method, const String& uri) override;
  void raw(WebServer& server, const String& uri, HTTPRaw& raw) override;
  void upload(WebServer& server, const String& uri, HTTPUpload& upload) override;
 private:
  bool parseAfter_(WebServer& server, uint16_t& after) const;
  bool parsePendingNoClassQuery_(WebServer& server, size_t& after) const;
  bool parseTodayQuery_(WebServer& server, uint8_t& session, uint16_t& after) const;
  bool parseDayScansQuery_(WebServer& server, int32_t& day,
                           uint8_t& session, uint32_t& cursor) const;
  bool parseDaySummaryQuery_(WebServer& server, int32_t& day,
                             uint8_t& session, uint16_t& after) const;
  bool parseHistoricalGridQuery_(WebServer& server, int32_t& fromDay,
                                 int32_t& toDay, uint16_t& after) const;
  bool buildPage_(uint16_t after);
  bool buildToday_(uint8_t session, uint16_t after);
  bool buildPendingAutomaticNoClass_(size_t after);
  bool buildDayScans_(int32_t day, uint8_t session, uint32_t cursor);
  bool buildDayAdjustments_(int32_t day, uint8_t session, uint32_t cursor);
  bool buildDaySummary_(int32_t day, uint8_t session, uint16_t after);
  bool prepareHistoricalRange_(int32_t fromDay, int32_t toDay, bool& ready);
  bool buildHistoricalGrid_(int32_t fromDay, int32_t toDay, uint16_t after);
  bool prepareDayView_(int32_t day, uint8_t session, bool& ready);
  bool buildTimetable_();
  bool appendLiteral_(const char* value);
  bool appendByte_(char value);
  bool appendFormat_(const char* format, ...);
  bool appendJsonString_(const char* value, size_t length);
  bool writeAll_(NetworkClient& client, const uint8_t* bytes, size_t length, uint32_t started);
  bool writeResponse_(WebServer& server, int status, const char* body, size_t length);
  void sendError_(WebServer& server, int status, const char* error);
  void receiveMutationBody_(WebServer& server, HTTPRaw& raw);
  void dispatchRosterMutation_(WebServer& server);
  void dispatchAttendanceMutation_(WebServer& server, const String& uri);
  void dispatchTodayStatusMutation_(WebServer& server);
  void dispatchAutomaticNoClassReviewMutation_(WebServer& server);
  void dispatchHistoricalMarkMutation_(WebServer& server);
  AdminSessionBoundary& sessions_;
  LogStore& log_;
  MonthSet& monthData_;
  RosterStore& roster_;
  DayView dayView_;
  uint32_t todayRevision_[MAX_ID + 1];
  uint32_t todayScanEpoch_[MAX_ID + 1];
  uint32_t todayLastStatusEpoch_[MAX_ID + 1];
  uint32_t todayLastStatusSequence_[MAX_ID + 1];
  int32_t dayCacheDay_;
  uint8_t dayCacheSession_;
  size_t dayCacheNextRecord_;
  size_t dayCacheRecordCount_;
  uint64_t dayCacheGeneration_;
  bool dayCacheInitialized_;
  bool dayCacheReady_;
  struct HistoricalColumn { int32_t day; uint8_t session; uint8_t viewIndex; };
  DayView historicalViews_[14];
  HistoricalColumn historicalColumns_[10];
  int32_t historicalFromDay_;
  int32_t historicalToDay_;
  size_t historicalNextRecord_;
  size_t historicalRecordCount_;
  uint64_t historicalGeneration_;
  uint8_t historicalColumnCount_;
  bool historicalInitialized_;
  bool historicalReady_;
  const Config& config_;
  HealthFsm& health_;
  MonotonicSecondsSource monotonicSeconds_;
  bool& applicationFaulted_;
  RosterEventWriter* writer_;
  Ds3231Clock* rtcClock_;
  ClockHealthOwner* clockHealth_;
  bool* storageWritesAuthorized_;
  AttendanceEventWriter* attendanceWriter_;
  const LogIndex* logIndex_;
  BoundedJsonBody mutationBody_;
  char response_[2048];
  size_t responseLength_;
};
}  // namespace att
