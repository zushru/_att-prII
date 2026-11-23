#pragma once
#include "log_store.h"
#include "status_derive.h"
#include "time_rules.h"
namespace att {
const uint16_t MAX_ID = 300;
struct DayView {
  int32_t day; uint8_t session;
  bool noClassDay, noClassSession, closed, coverage;
  bool manualNoClassDay, manualNoClassSession;
  bool autoNoClassPending, autoNoClassConfirmed;
  uint32_t autoNoClassSequence;
  uint16_t autoNoClassExpectedCount;
  uint32_t pendingAutoReviewTarget;
  int32_t pendingAutoReviewDay;
  uint8_t pendingAutoReviewSession;
  bool pendingAutoReview;
  uint16_t expectedCount;
  uint8_t scan[MAX_ID + 1];
  uint8_t mark[MAX_ID + 1];
  uint64_t expected[5];
  bool collecting; uint16_t pi;     // CLOSE payload collection state (persists across incremental applies)
};
void applyEvent(DayView& v, const Event& e, const Params& p);
// True only when the target view already has the desired EV_MARK or
// EV_NOCLASS state; malformed/mismatched events are never treated as no-ops.
bool dayViewAlreadyMatchesDesiredEvent(const DayView& view, const Event& desired);
// Full replay; firstSeq must match the LogStore constructor sequence origin.
bool buildDayView(LogStore& log, const Params& p, int32_t day, uint8_t session,
                  DayView& v, uint32_t firstSeq = 1);
Derived statusFor(const DayView& v, uint16_t id, bool liveExpected, bool started);
// Cached view for one (day, session). Call invalidate after a log reopen or append when not attached as an observer.
class DayCache : public LogObserver {
 public:
  DayCache(LogStore& log, const Params& p, uint32_t firstSeq = 1)
      : log_(log), p_(p), firstSeq_(firstSeq), v_(), day_(0), s_(0), dirty_(true) {}
  void target(int32_t day, uint8_t session);
  void invalidate() { dirty_ = true; }
  bool view(const DayView*& out);
  void onReset() override { dirty_ = true; }
  void onRecord(const Event& e) override { if (!dirty_) applyEvent(v_, e, p_); }
 private:
  LogStore& log_; Params p_; uint32_t firstSeq_; DayView v_;
  int32_t day_; uint8_t s_; bool dirty_;
};
}
