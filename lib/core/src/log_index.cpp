#include "log_index.h"
#include "event_codec.h"
#include "time_rules.h"
#include <string.h>
namespace att {
namespace {
}
LogIndex::LogIndex()
    : nPendingAutoNoClass_(0), nGaps_(0), nCloses_(0), nEvidence_(0),
      gapOverflow_(false), closeOverflow_(false), evidenceOverflow_(false),
      evidenceMalformed_(false), pendingAutoOverflow_(false),
      pendingAutoMalformed_(false),
      windowSet_(false), firstDay_(0), lastDay_(0), utcOffsetMin_(0),
      windowFrom_(0), windowTo_(0), overflowFrom_(0), overflowTo_(0),
      pendingAutoReview_(false), pendingAutoReviewDay_(0),
      pendingAutoReviewSession_(0), pendingAutoReviewTarget_(0) {}
void LogIndex::clearProjection_() {
  nGaps_ = 0;
  nCloses_ = 0;
  nEvidence_ = 0;
  nPendingAutoNoClass_ = 0;
  gapOverflow_ = false;
  closeOverflow_ = false;
  evidenceOverflow_ = false;
  evidenceMalformed_ = false;
  pendingAutoOverflow_ = false;
  pendingAutoMalformed_ = false;
  overflowFrom_ = overflowTo_ = 0;
  pendingAutoReview_ = false;
  pendingAutoReviewDay_ = 0;
  pendingAutoReviewSession_ = 0;
  pendingAutoReviewTarget_ = 0;
}
void LogIndex::reset_() { clearProjection_(); }
void LogIndex::onReset() { reset_(); }

void LogIndex::rememberGapOverflow_(uint64_t from, uint64_t to) {
  if (to <= from) return;
  if (!gapOverflow_) {
    overflowFrom_ = from;
    overflowTo_ = to;
  } else {
    if (from < overflowFrom_) overflowFrom_ = from;
    if (to > overflowTo_) overflowTo_ = to;
  }
  gapOverflow_ = true;
}

void LogIndex::insertGap_(uint32_t from, uint32_t to) {
  size_t first = 0;
  while (first < nGaps_ && gaps_[first].to < from) ++first;
  size_t last = first;
  while (last < nGaps_ && gaps_[last].from <= to) {
    if (gaps_[last].from < from) from = gaps_[last].from;
    if (gaps_[last].to > to) to = gaps_[last].to;
    ++last;
  }
  if (last > first) {
    gaps_[first] = Span{from, to};
    const size_t removed = last - first - 1;
    for (size_t i = last; i < nGaps_; ++i) gaps_[i - removed] = gaps_[i];
    nGaps_ -= removed;
  } else if (nGaps_ < MAX_GAPS) {
    for (size_t i = nGaps_; i > first; --i) gaps_[i] = gaps_[i - 1];
    gaps_[first] = Span{from, to};
    ++nGaps_;
  } else {
    rememberGapOverflow_(from, to);
  }
}

size_t LogIndex::findEvidence_(int32_t day, uint8_t session) const {
  size_t first = 0, last = nEvidence_;
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const SessionEvidence& item = evidence_[middle];
    if (item.day < day || (item.day == day && item.session < session))
      first = middle + 1;
    else last = middle;
  }
  return first < nEvidence_ && evidence_[first].day == day &&
      evidence_[first].session == session ? first : nEvidence_;
}
size_t LogIndex::findPendingAutoNoClass_(int32_t day, uint8_t session) const {
  size_t first = 0, last = nPendingAutoNoClass_;
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const PendingAutomaticNoClass& item = pendingAutoNoClass_[middle];
    if (item.day < day || (item.day == day && item.session < session))
      first = middle + 1;
    else last = middle;
  }
  return first < nPendingAutoNoClass_ &&
      pendingAutoNoClass_[first].day == day &&
      pendingAutoNoClass_[first].session == session
      ? first : nPendingAutoNoClass_;
}

bool LogIndex::addPendingAutoNoClass_(int32_t day, uint8_t session,
                                      uint32_t sequence,
                                      uint16_t expectedCount) {
  if (day < 0 || day > MAX_SUPPORTED_DAY || session > 1 || sequence == 0 ||
      expectedCount == 0 || findPendingAutoNoClass_(day, session) !=
          nPendingAutoNoClass_) {
    pendingAutoMalformed_ = true;
    return false;
  }
  if (nPendingAutoNoClass_ >= MAX_AUTO_NOCLASS_PENDING) {
    pendingAutoOverflow_ = true;
    return false;
  }
  size_t insertAt = 0;
  while (insertAt < nPendingAutoNoClass_ &&
      (pendingAutoNoClass_[insertAt].day < day ||
       (pendingAutoNoClass_[insertAt].day == day &&
        pendingAutoNoClass_[insertAt].session < session))) ++insertAt;
  for (size_t i = nPendingAutoNoClass_; i > insertAt; --i)
    pendingAutoNoClass_[i] = pendingAutoNoClass_[i - 1];
  PendingAutomaticNoClass pending = {};
  pending.declarationSequence = sequence;
  pending.day = static_cast<uint16_t>(day);
  pending.session = session;
  pending.expectedCount = expectedCount;
  pending.reserved = 0;
  pendingAutoNoClass_[insertAt] = pending;
  ++nPendingAutoNoClass_;
  return true;
}

bool LogIndex::removePendingAutoNoClass_(int32_t day, uint8_t session,
                                         uint32_t sequence) {
  const size_t at = findPendingAutoNoClass_(day, session);
  if (at == nPendingAutoNoClass_ ||
      pendingAutoNoClass_[at].declarationSequence != sequence) {
    pendingAutoMalformed_ = true;
    return false;
  }
  for (size_t i = at + 1; i < nPendingAutoNoClass_; ++i)
    pendingAutoNoClass_[i - 1] = pendingAutoNoClass_[i];
  --nPendingAutoNoClass_;
  return true;
}

size_t LogIndex::ensureEvidence_(int32_t day, uint8_t session) {
  if (session > 1 || (windowSet_ && (day < firstDay_ || day > lastDay_)))
    return nEvidence_;
  const size_t found = findEvidence_(day, session);
  if (found != nEvidence_) return found;
  size_t insertAt = 0;
  while (insertAt < nEvidence_ &&
      (evidence_[insertAt].day < day ||
       (evidence_[insertAt].day == day && evidence_[insertAt].session < session)))
    ++insertAt;
  if (nEvidence_ >= MAX_CLOSES) {
    evidenceOverflow_ = true;
    return nEvidence_;
  }
  for (size_t i = nEvidence_; i > insertAt; --i)
    evidence_[i] = evidence_[i - 1];
  SessionEvidence empty = {};
  empty.day = day;
  empty.session = session;
  evidence_[insertAt] = empty;
  ++nEvidence_;
  return insertAt;
}

void LogIndex::incrementSaturating_(uint16_t& value) {
  if (value != UINT16_MAX) ++value;
}

bool LogIndex::setWindow(int32_t firstDay, int32_t lastDay,
                         int16_t utcOffsetMin) {
  if (firstDay < 0 || lastDay < firstDay || lastDay > MAX_SUPPORTED_DAY ||
      utcOffsetMin < -720 || utcOffsetMin > 840) return false;
  const uint64_t from = dayStartEpoch(firstDay, utcOffsetMin);
  const uint64_t to = (uint64_t)dayStartEpoch(lastDay, utcOffsetMin) + 86400u;
  if (to <= from || to > (uint64_t)UINT32_MAX + 1u) return false;

  // Prune the entire bounded per-session projection as the active window rolls.
  size_t kept = 0;
  nCloses_ = 0;
  for (size_t i = 0; i < nEvidence_; ++i) {
    if (evidence_[i].day < firstDay || evidence_[i].day > lastDay) continue;
    evidence_[kept++] = evidence_[i];
    if (evidence_[i].closed) ++nCloses_;
  }
  nEvidence_ = kept;
  // Once evidence has overflowed, this instance cannot know which omitted facts
  // remain in the new window. Keep it fail-closed until a fresh replay.

  // Clip the existing merged GAP union to the active epoch interval.
  kept = 0;
  for (size_t i = 0; i < nGaps_; ++i) {
    const uint64_t clippedFrom = gaps_[i].from > from ? gaps_[i].from : from;
    const uint64_t clippedTo = gaps_[i].to < to ? gaps_[i].to : to;
    if (clippedTo > clippedFrom && clippedTo <= UINT32_MAX)
      gaps_[kept++] = Span{(uint32_t)clippedFrom, (uint32_t)clippedTo};
  }
  nGaps_ = kept;

  if (gapOverflow_ && overflowTo_ <= from) {
    gapOverflow_ = false;
    overflowFrom_ = overflowTo_ = 0;
  } else if (gapOverflow_) {
    if (overflowFrom_ < from) overflowFrom_ = from;
    if (overflowTo_ > to) overflowTo_ = to;
    if (overflowTo_ <= overflowFrom_) {
      gapOverflow_ = false;
      overflowFrom_ = overflowTo_ = 0;
    }
  }
  firstDay_ = firstDay;
  lastDay_ = lastDay;
  utcOffsetMin_ = utcOffsetMin;
  windowFrom_ = from;
  windowTo_ = to;
  windowSet_ = true;
  return true;
}

void LogIndex::onRecord(const Event& e) {
  if (e.type == EV_GAP) {
    const uint64_t rawFrom = e.epoch;
    const uint64_t rawTo = rawFrom + (uint64_t)e.b * 60u;
    if (windowSet_ && (rawTo <= windowFrom_ || rawFrom >= windowTo_))
      return;
    if (e.b == 0 || rawTo > UINT32_MAX) {
      if (windowSet_) rememberGapOverflow_(windowFrom_, windowTo_);
      else rememberGapOverflow_(rawFrom, rawTo > rawFrom ? rawTo : rawFrom + 1u);
      return;
    }
    uint64_t clippedFrom = rawFrom;
    uint64_t clippedTo = rawTo;
    if (windowSet_) {
      if (clippedFrom < windowFrom_) clippedFrom = windowFrom_;
      if (clippedTo > windowTo_) clippedTo = windowTo_;
    }
    if (clippedTo > clippedFrom && clippedTo <= UINT32_MAX)
      insertGap_((uint32_t)clippedFrom, (uint32_t)clippedTo);
    return;
  }

  if (e.type == EV_SCAN) {
    if (e.epoch < EPOCH_2020) { evidenceMalformed_ = true; return; }
    const int32_t day = dayNumber(e.epoch, utcOffsetMin_);
    const uint8_t session = flagSession(e.flags);
    const size_t at = ensureEvidence_(day, session);
    if (at != nEvidence_) incrementSaturating_(evidence_[at].acceptedScans);
    return;
  }

  if (e.type == EV_MARK || e.type == EV_TODAY_STATUS) {
    const int32_t day = static_cast<int32_t>(e.b);
    const uint8_t session = flagSession(e.flags);
    const size_t at = ensureEvidence_(day, session);
    if (at != nEvidence_) incrementSaturating_(evidence_[at].manualAttendanceEntries);
    return;
  }

  if (e.type == EV_NOCLASS_SET || e.type == EV_NOCLASS_CLEAR) {
    const bool active = e.type == EV_NOCLASS_SET;
    if (flagWholeDay(e.flags)) {
      for (uint8_t session = 0; session < 2; ++session) {
        const size_t at = ensureEvidence_(static_cast<int32_t>(e.b), session);
        if (at != nEvidence_) evidence_[at].manualNoClass = active;
      }
    } else {
      const size_t at = ensureEvidence_(static_cast<int32_t>(e.b), flagSession(e.flags));
      if (at != nEvidence_) {
        evidence_[at].manualNoClass = active;
        // Once confirmed, an administrator may later clear this session from
        // History using the existing reason-bearing EV_NOCLASS_CLEAR group.
        if (!active && evidence_[at].autoConfirmed)
          evidence_[at].autoConfirmed = false;
      }
    }
    return;
  }

  if (e.type == EV_CLOSE) {
    const int32_t day = static_cast<int32_t>(e.b);
    const uint8_t session = flagSession(e.flags);
    if (windowSet_ && (day < firstDay_ || day > lastDay_)) return;
    const size_t at = ensureEvidence_(day, session);
    if (at == nEvidence_) {
      closeOverflow_ = true;
      return;
    }
    SessionEvidence& item = evidence_[at];
    if (!item.closed) {
      item.closed = true;
      item.coverageComplete = flagCoverage(e.flags);
      item.expectedCount = e.a;
      ++nCloses_;
    } else {
      evidenceMalformed_ = true;  // duplicate CLOSE identity is not a trusted snapshot
    }
    if (nCloses_ > MAX_CLOSES) closeOverflow_ = true;
    return;
  }

  if (e.type == EV_AUTO_NOCLASS_SET) {
    const int32_t day = static_cast<int32_t>(e.b);
    const uint8_t session = flagSession(e.flags);
    if (e.b > MAX_SUPPORTED_DAY || session > 1 || e.a == 0 ||
        e.epoch < EPOCH_2020) {
      pendingAutoMalformed_ = true;
      return;
    }
    const bool inActiveWindow = !windowSet_ ||
        (day >= firstDay_ && day <= lastDay_);
    const size_t at = ensureEvidence_(day, session);
    if (inActiveWindow) {
      if (at == nEvidence_) return;
      SessionEvidence& item = evidence_[at];
      if (item.autoEvaluated || !item.closed || !item.coverageComplete ||
          item.expectedCount == 0 || item.acceptedScans != 0 ||
          item.manualAttendanceEntries != 0 || item.manualNoClass ||
          e.a != item.expectedCount) {
        evidenceMalformed_ = true;
        return;
      }
      if (!addPendingAutoNoClass_(day, session, e.seq, e.a)) return;
      item.autoEvaluated = true;
      item.autoPending = true;
      item.autoConfirmed = false;
      item.autoDeclarationSequence = e.seq;
    } else {
      // The compact pending ledger is independent of the rolling attendance
      // evidence window. An unresolved alert must not age out with register
      // history. The original close/eligibility checks were made before append;
      // later review still replays the exact day from the immutable log.
      (void)addPendingAutoNoClass_(day, session, e.seq, e.a);
    }
    return;
  }

  if (e.type == EV_TODAY_CAS && e.a == AUTO_NOCLASS_REVIEW_CAS_MAGIC &&
      (e.flags == FL_MORE || e.flags == (FL_MORE | FL_SESSION)) &&
      e.epoch != 0 && e.b <= MAX_SUPPORTED_DAY) {
    if (pendingAutoReview_) { evidenceMalformed_ = true; return; }
    pendingAutoReview_ = true;
    pendingAutoReviewDay_ = static_cast<int32_t>(e.b);
    pendingAutoReviewSession_ = flagSession(e.flags);
    pendingAutoReviewTarget_ = e.epoch;
    return;
  }

  if (e.type == EV_AUTO_NOCLASS_REVIEW) {
    if (!pendingAutoReview_ || e.epoch < EPOCH_2020 ||
        e.b != static_cast<uint32_t>(pendingAutoReviewDay_)) {
      evidenceMalformed_ = true;
      pendingAutoReview_ = false;
      return;
    }
    const uint8_t session = flagSession(e.flags);
    const int32_t day = static_cast<int32_t>(e.b);
    if (session != pendingAutoReviewSession_ ||
        pendingAutoReviewTarget_ == 0 || (e.a != 1 && e.a != 2) ||
        !removePendingAutoNoClass_(day, session,
                                   pendingAutoReviewTarget_)) {
      evidenceMalformed_ = true;
    } else {
      const size_t at = findEvidence_(day, session);
      if (at != nEvidence_) {
        if (evidence_[at].autoDeclarationSequence != pendingAutoReviewTarget_ ||
            !evidence_[at].autoPending) {
          evidenceMalformed_ = true;
        } else if (e.a == 1) {
          evidence_[at].autoPending = false;
          evidence_[at].autoConfirmed = true;
        } else {
          evidence_[at].autoPending = false;
          evidence_[at].autoConfirmed = false;
        }
      }
    }
    pendingAutoReview_ = false;
    pendingAutoReviewDay_ = 0;
    pendingAutoReviewSession_ = 0;
    pendingAutoReviewTarget_ = 0;
    return;
  }
}

bool LogIndex::closed(int32_t day, uint8_t session) const {
  const size_t at = findEvidence_(day, session);
  return at != nEvidence_ && evidence_[at].closed;
}

bool LogIndex::sessionEvidence(int32_t day, uint8_t session,
                               SessionEvidence& out) const {
  if (session > 1) return false;
  const size_t at = findEvidence_(day, session);
  if (at == nEvidence_) return false;
  out = evidence_[at];
  return true;
}

bool LogIndex::sessionEvidenceAt(size_t index, SessionEvidence& out) const {
  if (index >= nEvidence_) return false;
  out = evidence_[index];
  return true;
}
bool LogIndex::pendingAutoNoClassAt(
    size_t index, PendingAutomaticNoClass& out) const {
  if (index >= nPendingAutoNoClass_) return false;
  out = pendingAutoNoClass_[index];
  return true;
}

bool LogIndex::nextAutoNoClassCandidate(SessionEvidence& out) const {
  if (!evidenceHealthy()) return false;
  for (size_t i = 0; i < nEvidence_; ++i) {
    const SessionEvidence& item = evidence_[i];
    if (item.closed && item.coverageComplete && item.expectedCount > 0 &&
        item.acceptedScans == 0 && item.manualAttendanceEntries == 0 &&
        !item.manualNoClass && !item.autoEvaluated) {
      out = item;
      return true;
    }
  }
  return false;
}

bool LogIndex::gapOverlaps(uint32_t ws, uint32_t we) const {
  if (we <= ws) return false;
  size_t first = 0, last = nGaps_;
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    if (gaps_[middle].to <= ws) first = middle + 1;
    else last = middle;
  }
  return first < nGaps_ && gaps_[first].from < we && gaps_[first].to > ws;
}
}
