#include "day_view.h"
#include <stdint.h>
#include <string.h>
namespace att {
void applyEvent(DayView& v, const Event& e, const Params& p) {
  if (e.type == EV_PAYLOAD) {
    if (v.collecting) {
      if (v.pi < 5) v.expected[v.pi++] = payloadBits(e);
      if (!flagMore(e.flags)) v.collecting = false;
    }
    return;
  }
  v.collecting = false;
  bool sess = flagSession(e.flags) == v.session;
  bool idOk = e.a >= 1 && e.a <= MAX_ID;
  switch (e.type) {
    case EV_SCAN:
      if (sess && idOk && v.scan[e.a] == 0 && dayNumber(e.epoch, p.utcOffsetMin) == v.day) {
        uint8_t st = flagStatus(e.flags);
        if (st == ST_PRESENT || st == ST_LATE) v.scan[e.a] = st;
      }
      break;
    case EV_MARK:
    case EV_TODAY_STATUS:
      if (sess && idOk && e.b == v.day) v.mark[e.a] = flagStatus(e.flags);
      break;
    case EV_NOCLASS_SET:
    case EV_NOCLASS_CLEAR:
      if (e.b == v.day) {
        const bool on = e.type == EV_NOCLASS_SET;
        if (flagWholeDay(e.flags)) {
          v.manualNoClassDay = on;
          v.noClassDay = on;
        } else if (sess) {
          v.manualNoClassSession = on;
          // A reason-bearing History clear is the only post-confirmation
          // override. Pending automatic results must first be reviewed.
          if (!on && v.autoNoClassConfirmed) v.autoNoClassConfirmed = false;
          v.noClassSession = on || v.autoNoClassPending || v.autoNoClassConfirmed;
        }
      }
      break;
    case EV_AUTO_NOCLASS_SET:
      if (e.b == v.day && sess && !v.autoNoClassPending &&
          !v.autoNoClassConfirmed && e.a >= 1 && e.a <= MAX_ID &&
          e.epoch >= EPOCH_2020) {
        v.autoNoClassPending = true;
        v.autoNoClassSequence = e.seq;
        v.autoNoClassExpectedCount = e.a;
        v.noClassSession = true;
      }
      break;
    case EV_TODAY_CAS:
      if (e.a == AUTO_NOCLASS_REVIEW_CAS_MAGIC &&
          (e.flags == FL_MORE || e.flags == (FL_MORE | FL_SESSION)) &&
          e.epoch != 0 && e.b <= MAX_SUPPORTED_DAY) {
        v.pendingAutoReview = true;
        v.pendingAutoReviewTarget = e.epoch;
        v.pendingAutoReviewDay = static_cast<int32_t>(e.b);
        v.pendingAutoReviewSession = flagSession(e.flags);
      }
      break;
    case EV_AUTO_NOCLASS_REVIEW:
      if (v.pendingAutoReview && e.b == v.day && sess &&
          v.pendingAutoReviewDay == v.day &&
          v.pendingAutoReviewSession == v.session &&
          v.pendingAutoReviewTarget != 0 &&
          v.pendingAutoReviewTarget == v.autoNoClassSequence &&
          v.autoNoClassPending && e.epoch >= EPOCH_2020) {
        if (e.a == ANCR_CONFIRM) {
          v.autoNoClassPending = false;
          v.autoNoClassConfirmed = true;
        } else if (e.a == ANCR_UNDO) {
          v.autoNoClassPending = false;
          v.autoNoClassConfirmed = false;
        }
        v.noClassSession = v.manualNoClassSession ||
            v.autoNoClassPending || v.autoNoClassConfirmed;
      }
      v.pendingAutoReview = false;
      v.pendingAutoReviewTarget = 0;
      v.pendingAutoReviewDay = 0;
      v.pendingAutoReviewSession = 0;
      break;
    case EV_CLOSE:
      if (sess && e.b == v.day && !v.closed) {
        v.closed = true; v.coverage = flagCoverage(e.flags); v.expectedCount = e.a;
        v.collecting = flagMore(e.flags); v.pi = 0;
      }
      break;
    default: break;
  }
}
bool dayViewAlreadyMatchesDesiredEvent(const DayView& view,
                                       const Event& desired) {
  if (desired.b != view.day) return false;
  if (desired.type == EV_MARK) {
    if (desired.a < 1 || desired.a > MAX_ID ||
        flagSession(desired.flags) != view.session ||
        flagStatus(desired.flags) > ST_EXCUSED) return false;
    return view.mark[desired.a] == flagStatus(desired.flags);
  }
  if (desired.type == EV_NOCLASS_SET || desired.type == EV_NOCLASS_CLEAR) {
    const bool wanted = desired.type == EV_NOCLASS_SET;
    if (flagWholeDay(desired.flags)) return view.noClassDay == wanted;
    if (flagSession(desired.flags) != view.session) return false;
    return view.noClassSession == wanted;
  }
  return false;
}
bool buildDayView(LogStore& log, const Params& p, int32_t day, uint8_t session,
                  DayView& v, uint32_t firstSeq) {
  memset(&v, 0, sizeof v);
  if (firstSeq == 0 || log.faulted()) return false;
  v.day = day; v.session = session;
  const size_t total = log.count();
  Event e;
  for (size_t i = 0; i < total; i++) {
    if ((uint64_t)firstSeq + (uint64_t)i > UINT32_MAX ||
        !log.at(i, e) || e.seq != firstSeq + (uint32_t)i) return false;
    applyEvent(v, e, p);
  }
  return !log.faulted() && log.count() == total;
}
Derived statusFor(const DayView& v, uint16_t id, bool liveExpected, bool started) {
  if (id < 1 || id > MAX_ID) return D_NA;
  Facts f;
  f.noClass = v.noClassDay || v.noClassSession;
  f.expected = v.closed ? bitmapTest(v.expected, id) : liveExpected;
  f.started = started; f.closed = v.closed; f.coverage = v.coverage;
  f.markStatus = v.mark[id]; f.scanStatus = v.scan[id];
  return derive(f);
}
void DayCache::target(int32_t day, uint8_t s) {
  if (day != day_ || s != s_) { day_ = day; s_ = s; dirty_ = true; }
}
bool DayCache::view(const DayView*& out) {
  if (dirty_) {
    if (!buildDayView(log_, p_, day_, s_, v_, firstSeq_)) return false;
    dirty_ = false;
  }
  out = &v_;
  return true;
}
}
