#include "event_group_audit.h"
#include "event_codec.h"
#include "day_view.h"
#include "roster_store.h"

#include <string.h>

namespace att {
namespace {
uint16_t bitCount64(uint64_t value) {
  uint16_t count = 0;
  while (value) {
    value &= value - 1;
    ++count;
  }
  return count;
}

bool validTodayStatusEvent(const Event& event) {
  const uint8_t allowed = static_cast<uint8_t>(FL_SESSION | 0x06u | FL_STATUS_EXT);
  if (event.type != EV_TODAY_STATUS || event.a < 1 || event.a > MAX_ID ||
      event.epoch == 0 || event.b > MAX_SUPPORTED_DAY ||
      flagMore(event.flags) || (event.flags & static_cast<uint8_t>(~allowed)) != 0)
    return false;
  const uint8_t status = flagStatus(event.flags);
  if (status != ST_PRESENT && status != ST_LATE &&
      status != ST_PENDING && status != ST_EXCUSED)
    return false;
  // Pending is the only extended code and must not carry the legacy low bits.
  if (status == ST_PENDING && (event.flags & 0x06u) != 0) return false;
  if (status != ST_PENDING && (event.flags & FL_STATUS_EXT) != 0) return false;
  return true;
}

bool validAutoNoClassSetEvent(const Event& event) {
  return event.type == EV_AUTO_NOCLASS_SET && event.a >= 1 &&
      event.a <= MAX_ID && event.epoch >= EPOCH_2020 &&
      event.b <= MAX_SUPPORTED_DAY &&
      (event.flags == 0 || event.flags == FL_SESSION);
}

bool validAutoNoClassReviewEvent(const Event& event) {
  return event.type == EV_AUTO_NOCLASS_REVIEW &&
      (event.a == ANCR_CONFIRM || event.a == ANCR_UNDO) &&
      event.epoch >= EPOCH_2020 && event.b <= MAX_SUPPORTED_DAY &&
      (event.flags == 0 || event.flags == FL_SESSION);
}

bool validManualAttendanceEvent(const Event& event) {
  if (flagMore(event.flags) || (event.flags & 0xC0u) != 0) return false;
  const uint8_t sessionStatusFlags = (uint8_t)(FL_SESSION | 0x06u);
  if (event.type == EV_MARK) {
    return event.a >= 1 && event.a <= MAX_ID && event.epoch != 0 &&
        (event.flags & (uint8_t)~sessionStatusFlags) == 0 &&
        flagStatus(event.flags) <= ST_EXCUSED;
  }
  if (event.type == EV_NOCLASS_SET || event.type == EV_NOCLASS_CLEAR) {
    const uint8_t noClassFlags = (uint8_t)(FL_SESSION | FL_WHOLEDAY);
    return event.a == 0 && event.epoch != 0 &&
        (event.flags & (uint8_t)~noClassFlags) == 0;
  }
  return false;
}
}  // namespace

EventGroupAudit::EventGroupAudit(uint32_t firstSeq)
    : firstSeq_(firstSeq), count_(0), malformed_(false), sequenceError_(false),
      attached_(false), recordsSeen_(0), lastSeq_(0), generation_(0),
      mutationBloom_{0},
      clockRequestPending_(false), pendingClockPairCommitted_(false),
      pendingClockRequestId_{0, 0, 0}, pendingClockRequestEpoch_(0),
      pendingClockPreviousEpoch_(0), completedClockRequestSeen_(false),
      completedClockRequestId_{0, 0, 0}, completedClockRequestEpoch_(0) {}

bool EventGroupAudit::attach(LogStore& log) {
  if (attached_ || log.hasObserver(this) || log.faulted()) return false;
  if (!log.attach(this)) return false;
  attached_ = true;
  return true;
}

bool EventGroupAudit::readyFor(const LogStore& log, const OpenInfo& opened) const {
  return attached_ && log.hasObserver(this) && opened.ok && opened.generation != 0 &&
      opened.generation == log.openGeneration() && opened.corruptAt < 0 &&
      opened.tornBytes == 0 && opened.droppedGroup == 0 && !log.faulted() &&
      !malformed_ && !pending() && !sequenceError_ &&
      recordsSeen_ == log.count() && lastSeq_ == log.lastSeq();
}

void EventGroupAudit::onReset() {
  count_ = 0;
  malformed_ = false;
  sequenceError_ = false;
  recordsSeen_ = 0;
  lastSeq_ = firstSeq_ == 0 ? 0 : firstSeq_ - 1u;
  clockRequestPending_ = false;
  pendingClockPairCommitted_ = false;
  memset(pendingClockRequestId_, 0, sizeof pendingClockRequestId_);
  pendingClockRequestEpoch_ = 0;
  pendingClockPreviousEpoch_ = 0;
  pendingClockGapRequired_ = false;
  pendingClockGapCursor_ = 0;
  pendingClockGapTarget_ = 0;
  completedClockRequestSeen_ = false;
  memset(completedClockRequestId_, 0, sizeof completedClockRequestId_);
  completedClockRequestEpoch_ = 0;
  memset(mutationBloom_, 0, sizeof mutationBloom_);
  ++generation_;
}

void EventGroupAudit::onRecord(const Event& event) {
  if (malformed_) return;
  if (firstSeq_ == 0 || lastSeq_ == UINT32_MAX ||
      event.seq != lastSeq_ + 1u || recordsSeen_ == UINT32_MAX) {
    sequenceError_ = true;
    malformed_ = true;
    count_ = 0;
    return;
  }
  ++recordsSeen_;
  lastSeq_ = event.seq;

  if (count_ >= MAX_GROUP) {
    malformed_ = true;
    count_ = 0;
    return;
  }
  group_[count_++] = event;
  if (!flagMore(event.flags)) {
    if (!validGroup_() || !trackClockJournal_()) {
      malformed_ = true;
    } else if (group_[0].type == EV_ADMIN_MUTATION) {
      rememberAdminMutation_(group_[0]);
    }
    count_ = 0;
  }
}

void EventGroupAudit::rememberAdminMutation_(const Event& marker) {
  // FNV-1a over the exact 64-bit identity stored across a/b/epoch.
  uint32_t h = 2166136261u;
  const uint32_t values[3] = {
      static_cast<uint32_t>(marker.a),
      static_cast<uint32_t>(marker.b), marker.epoch};
  for (size_t i = 0; i < 3; ++i) {
    for (uint8_t shift = 0; shift < 32; shift += 8) {
      h ^= static_cast<uint8_t>(values[i] >> shift);
      h *= 16777619u;
    }
  }
  // Two independently avalanched positions. Bloom collisions only cause an
  // exact durable scan; they can never authorize a replay or suppress a write.
  uint32_t h2 = h ^ 0x9e3779b9u;
  h2 ^= h2 >> 16; h2 *= 0x7feb352du;
  h2 ^= h2 >> 15; h2 *= 0x846ca68bu;
  h2 ^= h2 >> 16;
  const uint16_t bits[3] = {
      static_cast<uint16_t>(h & 8191u),
      static_cast<uint16_t>(h2 & 8191u),
      static_cast<uint16_t>((h + h2 + 0x85ebca6bu) & 8191u)};
  for (size_t i = 0; i < 3; ++i)
    mutationBloom_[bits[i] >> 3] |= static_cast<uint8_t>(1u << (bits[i] & 7u));
}

bool EventGroupAudit::mayContainAdminMutation(
    const uint16_t requestIdWords[4]) const {
  if (requestIdWords == nullptr) return true;  // invalid input fails toward exact scan
  if (requestIdWords[0] == 0 && requestIdWords[1] == 0 &&
      requestIdWords[2] == 0 && requestIdWords[3] == 0) return true;
  const Event marker = Event{EV_ADMIN_MUTATION, FL_MORE,
      requestIdWords[0],
      (static_cast<uint32_t>(requestIdWords[2]) << 16) |
          static_cast<uint32_t>(requestIdWords[3]),
      0, requestIdWords[1]};
  uint32_t h = 2166136261u;
  const uint32_t values[3] = {static_cast<uint32_t>(marker.a),
      static_cast<uint32_t>(marker.b), marker.epoch};
  for (size_t i = 0; i < 3; ++i) {
    for (uint8_t shift = 0; shift < 32; shift += 8) {
      h ^= static_cast<uint8_t>(values[i] >> shift);
      h *= 16777619u;
    }
  }
  uint32_t h2 = h ^ 0x9e3779b9u;
  h2 ^= h2 >> 16; h2 *= 0x7feb352du;
  h2 ^= h2 >> 15; h2 *= 0x846ca68bu;
  h2 ^= h2 >> 16;
  const uint16_t bits[3] = {
      static_cast<uint16_t>(h & 8191u),
      static_cast<uint16_t>(h2 & 8191u),
      static_cast<uint16_t>((h + h2 + 0x85ebca6bu) & 8191u)};
  for (size_t i = 0; i < 3; ++i)
    if ((mutationBloom_[bits[i] >> 3] &
         static_cast<uint8_t>(1u << (bits[i] & 7u))) == 0) return false;
  return true;
}

bool EventGroupAudit::trackClockJournal_() {
  if (count_ == 0) return false;
  const Event& first = group_[0];

  if (first.type == EV_SETTING) {
    const bool intent = first.flags == (FL_MORE | FL_SESSION);
    const uint16_t id[3] = {first.a, first.b, group_[1].a};
    if (intent) {
      if (clockRequestPending_) return false;
      clockRequestPending_ = true;
      pendingClockPairCommitted_ = false;
      memcpy(pendingClockRequestId_, id, sizeof id);
      pendingClockRequestEpoch_ = first.epoch;
      pendingClockPreviousEpoch_ = group_[1].epoch;
      const uint32_t delta = pendingClockPreviousEpoch_ > pendingClockRequestEpoch_
          ? pendingClockPreviousEpoch_ - pendingClockRequestEpoch_
          : pendingClockRequestEpoch_ - pendingClockPreviousEpoch_;
      pendingClockGapRequired_ = delta > 120u;
      pendingClockGapCursor_ = pendingClockGapRequired_
          ? (pendingClockPreviousEpoch_ < pendingClockRequestEpoch_
              ? pendingClockPreviousEpoch_ : pendingClockRequestEpoch_) : 0;
      pendingClockGapTarget_ = pendingClockGapRequired_
          ? (pendingClockPreviousEpoch_ > pendingClockRequestEpoch_
              ? pendingClockPreviousEpoch_ : pendingClockRequestEpoch_) : 0;
      return true;
    }

    if (!clockRequestPending_ || !pendingClockPairCommitted_ ||
        id[0] != pendingClockRequestId_[0] ||
        id[1] != pendingClockRequestId_[1] ||
        id[2] != pendingClockRequestId_[2] ||
        first.epoch != pendingClockRequestEpoch_ ||
        group_[1].epoch != pendingClockRequestEpoch_) return false;
    completedClockRequestSeen_ = true;
    memcpy(completedClockRequestId_, pendingClockRequestId_,
           sizeof completedClockRequestId_);
    completedClockRequestEpoch_ = pendingClockRequestEpoch_;
    clockRequestPending_ = false;
    pendingClockPairCommitted_ = false;
    memset(pendingClockRequestId_, 0, sizeof pendingClockRequestId_);
    pendingClockRequestEpoch_ = 0;
    pendingClockPreviousEpoch_ = 0;
    pendingClockGapRequired_ = false;
    pendingClockGapCursor_ = 0;
    pendingClockGapTarget_ = 0;
    return true;
  }

  if (!clockRequestPending_) return true;
  if (first.type == EV_GAP) {
    if (first.a != GAP_CLOCK || pendingClockPairCommitted_ ||
        !pendingClockGapRequired_) return false;
    const bool hasClockPair = count_ >= 2 &&
        group_[count_ - 2].type == EV_CLOCK_SET &&
        group_[count_ - 1].type == EV_CLOCK_SET;
    const size_t gapCount = hasClockPair ? count_ - 2 : count_;
    if (gapCount == 0) return false;
    for (size_t i = 0; i < gapCount; ++i) {
      const Event& gap = group_[i];
      if (gap.type != EV_GAP || gap.a != GAP_CLOCK || gap.epoch != pendingClockGapCursor_ ||
          gap.b == 0) return false;
      const uint64_t end = static_cast<uint64_t>(gap.epoch) +
          static_cast<uint64_t>(gap.b) * 60u;
      if (end > UINT32_MAX ||
          end > static_cast<uint64_t>(pendingClockGapTarget_) + 59u) return false;
      pendingClockGapCursor_ = static_cast<uint32_t>(end);
    }
    if (!hasClockPair) return pendingClockGapCursor_ < pendingClockGapTarget_;
    if (pendingClockGapCursor_ < pendingClockGapTarget_ ||
        pendingClockGapCursor_ - pendingClockGapTarget_ >= 60u) return false;
    const Event& oldEvent = group_[count_ - 2];
    const Event& newEvent = group_[count_ - 1];
    const uint16_t expectedSource = pendingClockPreviousEpoch_ == 0
        ? SRC_WIZARD : SRC_MANUAL;
    if (oldEvent.a != expectedSource || newEvent.a != expectedSource ||
        oldEvent.epoch != pendingClockPreviousEpoch_ ||
        newEvent.epoch != pendingClockRequestEpoch_) return false;
    pendingClockPairCommitted_ = true;
    return true;
  }

  if (first.type == EV_CLOCK_SET) {
    if (pendingClockPairCommitted_ || pendingClockGapRequired_ || count_ != 2) return false;
    const Event& newEvent = group_[1];
    const uint16_t expectedSource = pendingClockPreviousEpoch_ == 0
        ? SRC_WIZARD : SRC_MANUAL;
    if (newEvent.type != EV_CLOCK_SET || first.a != expectedSource ||
        newEvent.a != expectedSource ||
        first.epoch != pendingClockPreviousEpoch_ ||
        newEvent.epoch != pendingClockRequestEpoch_) return false;
    pendingClockPairCommitted_ = true;
    return true;
  }

  // Between durable intent and completion, only clock GAP coverage and the
  // exact matching CLOCK_SET pair are permitted to enter the event stream.
  return false;
}

bool EventGroupAudit::validGroup_() const {
  if (count_ == 0) return false;
  const Event& first = group_[0];
  for (size_t i = 1; i < count_; ++i) {
    if (group_[i].seq != group_[0].seq + (uint32_t)i) return false;
  }
  const uint8_t firstBase = (uint8_t)(first.flags & (uint8_t)~FL_MORE);

  if (first.type == EV_ADMIN_MUTATION) {
    if (first.flags != FL_MORE ||
        (first.a == 0 && first.b == 0 && first.epoch == 0)) return false;
    // Old firmware wrote [identity, operation]. Accept these groups so an
    // upgrade never makes already-committed attendance unreadable.
    if (count_ == 2) return validManualAttendanceEvent(group_[1]);
    if (count_ != 3 && count_ != 4 && count_ != 6) return false;
    const Event& audit = group_[1];
    const uint64_t sessionFingerprint =
        static_cast<uint64_t>(audit.epoch) |
        (static_cast<uint64_t>(audit.b) << 32);
    if (audit.type != EV_ADMIN_AUDIT || audit.flags != FL_MORE ||
        sessionFingerprint == 0) return false;
    if (audit.a == AMR_NONE) {
      if (count_ == 6 && group_[2].a == AUTO_NOCLASS_REVIEW_CAS_MAGIC) {
        // Automatic review stores the full request snapshot across three CAS
        // records. This lets exact-retry validation survive reboot and rejects
        // request-ID reuse with a changed generation or record count.
        const Event& cas0 = group_[2];
        const Event& cas1 = group_[3];
        const Event& cas2 = group_[4];
        const Event& operation = group_[5];
        const uint32_t expectedRecords = static_cast<uint32_t>(cas1.a) |
            (static_cast<uint32_t>(cas1.b) << 16);
        const uint64_t generation =
            (static_cast<uint64_t>(cas1.epoch) << 32) | cas2.epoch;
        const bool casValid = cas0.type == EV_TODAY_CAS &&
            (cas0.flags == FL_MORE || cas0.flags == (FL_MORE | FL_SESSION)) &&
            cas0.epoch != 0 && cas0.epoch < first.seq &&
            cas0.epoch <= expectedRecords && cas0.b <= MAX_SUPPORTED_DAY &&
            cas1.type == EV_TODAY_CAS && cas1.flags == (FL_MORE | 0x02u) &&
            cas2.type == EV_TODAY_CAS && cas2.flags == (FL_MORE | 0x04u) &&
            cas2.a == 0 && cas2.b == 0 && generation != 0 &&
            operation.seq == cas2.seq + 1u;
        return casValid && operation.b == cas0.b &&
            flagSession(operation.flags) == flagSession(cas0.flags) &&
            validAutoNoClassReviewEvent(operation);
      }
      // New Today writes are a six-record atomic group:
      // identity, reasonless audit, three exact request-CAS payload records,
      // and the status operation. Validate both framing and identity fields;
      // metadata must never be accepted as a standalone event.
      if (count_ != 6) return false;
      const Event& cas0 = group_[2];
      const Event& cas1 = group_[3];
      const Event& cas2 = group_[4];
      const Event& operation = group_[5];
      const uint8_t status = static_cast<uint8_t>((cas0.epoch >> 24) & 0xFFu);
      const uint16_t day = static_cast<uint16_t>(cas0.epoch & 0xFFFFu);
      const uint8_t session = static_cast<uint8_t>((cas0.epoch >> 16) & 0xFFu);
      const bool validStatus = status == ST_PRESENT || status == ST_PENDING ||
          status == ST_EXCUSED;
      const bool validCas = cas0.type == EV_TODAY_CAS && cas0.flags == FL_MORE &&
          cas0.a >= 1 && cas0.a <= MAX_ID && cas0.b == 0 &&
          day <= MAX_SUPPORTED_DAY && session <= 1 && validStatus &&
          cas1.type == EV_TODAY_CAS && cas1.flags == (FL_MORE | 0x02u) &&
          cas2.type == EV_TODAY_CAS && cas2.flags == (FL_MORE | 0x04u);
      const uint8_t committedStatus = flagStatus(operation.flags);
      const bool statusMatches = status == ST_PRESENT
          ? (committedStatus == ST_PRESENT || committedStatus == ST_LATE)
          : committedStatus == status;
      const bool matchingOperation = validTodayStatusEvent(operation) &&
          operation.a == cas0.a && operation.b == day &&
          flagSession(operation.flags) == session && statusMatches;
      const bool nonzeroSnapshot = cas2.a != 0 || cas2.epoch != 0 || cas2.b != 0;
      return validCas && matchingOperation && nonzeroSnapshot;
    }
    if (count_ != 3 || audit.a < AMR_DATA_ENTRY_ERROR ||
        audit.a > AMR_OTHER_DOCUMENTED) return false;
    return validManualAttendanceEvent(group_[2]);
  }

  if (first.type == EV_ROSTER) {
    if (firstBase != 0 || first.a < 1 || first.a > MAX_ID) return false;
    if (first.b == RA_SET) {
      if (count_ != 5 || !flagMore(first.flags)) return false;
      for (size_t i = 1; i < count_; ++i) {
        if (group_[i].type != EV_PAYLOAD ||
            (group_[i].flags & (uint8_t)~FL_MORE) != 0 ||
            flagMore(group_[i].flags) != (i + 1 < count_)) return false;
      }
      return true;
    }
    return count_ == 1 && !flagMore(first.flags) &&
        (first.b == RA_DEACTIVATE || first.b == RA_ACTIVATE);
  }

  if (first.type == EV_CLOSE) {
    // The signed product capacity is 300 IDs. The current writer always emits
    // exactly ceil(300/64) = five bitmap payload records, even for count zero.
    const uint8_t closeFlags = (uint8_t)(FL_SESSION | FL_COVERAGE | FL_MORE);
    if (count_ != 6 || first.a > MAX_ID ||
        (first.flags & (uint8_t)~closeFlags) != 0 || !flagMore(first.flags)) return false;
    uint16_t bitmapCount = 0;
    for (size_t i = 1; i < count_; ++i) {
      if (group_[i].type != EV_PAYLOAD ||
          (group_[i].flags & (uint8_t)~FL_MORE) != 0 ||
          flagMore(group_[i].flags) != (i + 1 < count_)) return false;
      const uint64_t bits = payloadBits(group_[i]);
      const size_t firstIdOffset = (i - 1) * 64;
      const size_t validBits = MAX_ID > firstIdOffset ?
          ((MAX_ID - firstIdOffset) < 64 ? (MAX_ID - firstIdOffset) : 64) : 0;
      if (validBits < 64 && (bits >> validBits) != 0) return false;
      bitmapCount = (uint16_t)(bitmapCount + bitCount64(bits));
    }
    return bitmapCount == first.a;
  }

  if (first.type == EV_GAP) {
    const bool hasClockPair = count_ >= 2 &&
        group_[count_ - 2].type == EV_CLOCK_SET &&
        group_[count_ - 1].type == EV_CLOCK_SET;
    const size_t gapCount = hasClockPair ? count_ - 2 : count_;
    if (gapCount == 0) return false;
    const uint16_t cause = group_[0].a;
    if (cause < GAP_POWER || cause > GAP_CLOCK) return false;
    for (size_t i = 0; i < gapCount; ++i) {
      const Event& e = group_[i];
      const uint64_t gapEnd = (uint64_t)e.epoch + (uint64_t)e.b * 60u;
      if (e.type != EV_GAP || e.a != cause || e.b == 0 ||
          gapEnd > UINT32_MAX ||
          (e.flags & (uint8_t)~FL_MORE) != 0 ||
          flagMore(e.flags) != (hasClockPair || i + 1 < gapCount)) return false;
      if (i + 1 < gapCount && gapEnd != group_[i + 1].epoch) return false;
    }
    if (hasClockPair) {
      const Event& a = group_[count_ - 2];
      const Event& b = group_[count_ - 1];
      return a.a == b.a && a.a <= SRC_WIZARD && a.b == 0 && b.b == 0 &&
          (a.flags & (uint8_t)~FL_MORE) == 0 &&
          (b.flags & (uint8_t)~FL_MORE) == 0 &&
          flagMore(a.flags) && !flagMore(b.flags);
    }
    return true;
  }

  if (first.type == EV_SETTING) {
    if (count_ != 2 || group_[1].type != EV_SETTING ||
        (first.flags != (FL_MORE | FL_SESSION) &&
         first.flags != (FL_MORE | FL_COVERAGE)) ||
        group_[1].flags != 0 || group_[1].b != 0 ||
        (first.a == 0 && first.b == 0 && group_[1].a == 0)) return false;
    const bool intent = first.flags == (FL_MORE | FL_SESSION);
    if (first.epoch < 1577836800UL) return false;
    if (intent) return group_[1].epoch == 0 || group_[1].epoch >= 1577836800UL;
    return group_[1].epoch == first.epoch;
  }

  if (first.type == EV_CLOCK_SET) {
    if (count_ != 2) return false;
    const Event& second = group_[1];
    return second.type == EV_CLOCK_SET && first.a == second.a &&
        first.a <= SRC_WIZARD && first.b == 0 && second.b == 0 &&
        (first.flags & (uint8_t)~FL_MORE) == 0 &&
        (second.flags & (uint8_t)~FL_MORE) == 0 &&
        flagMore(first.flags) && !flagMore(second.flags);
  }

  // PAYLOAD must be owned by a ROSTER SET or CLOSE group. Validate the current
  // single-record attendance writers as well as their commit-group shape.
  if (count_ != 1 || first.type == EV_PAYLOAD || flagMore(first.flags) ||
      (first.flags & 0xC0u) != 0) return false;

  const uint8_t sessionStatusFlags = (uint8_t)(FL_SESSION | 0x06u);
  if (first.type == EV_SCAN) {
    const uint8_t status = flagStatus(first.flags);
    return first.a >= 1 && first.a <= MAX_ID && first.epoch != 0 &&
        (first.flags & (uint8_t)~sessionStatusFlags) == 0 &&
        (status == ST_PRESENT || status == ST_LATE);
  }
  if (first.type == EV_MARK || first.type == EV_NOCLASS_SET ||
      first.type == EV_NOCLASS_CLEAR) return validManualAttendanceEvent(first);
  if (first.type == EV_AUTO_NOCLASS_SET)
    return validAutoNoClassSetEvent(first);
  // EV_BOOT, EV_REPAIR and any future event types have no current writer/schema
  // in this release. Refuse them rather than assuming unknown semantics are safe.
  return false;
}

}  // namespace att
