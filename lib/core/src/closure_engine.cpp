#include "closure_engine.h"
#include "config_store.h"
namespace att {
namespace {
bool validRosterCapacity(uint16_t cap) {
  return cap >= 1 && cap <= MAX_ID;
}
bool validClosureSchedule(const Params& p, const Timetable& t, int32_t goLiveDay) {
  if (goLiveDay < 0 || goLiveDay > MAX_SUPPORTED_DAY) return false;
  if (p.utcOffsetMin < -720 || p.utcOffsetMin > 840 ||
      p.scanLeadMin > 60 || p.lateMin > 240) return false;
  if (!validateTimetable(t, p.scanLeadMin)) return false;
  for (int day = 0; day < 5; ++day) {
    for (int session = 0; session < 2; ++session) {
      const Slot& slot = t.day[day][session];
      if (slot.enabled && p.lateMin >= slot.end - slot.start) return false;
    }
  }
  return true;
}
}  // namespace
ClosureEngine::ClosureEngine(LogStore& log, const Params& p, const Timetable& t,
    IRoster& r, uint16_t cap, int32_t goLiveDay, int32_t initialTodayDay)
    : log_(log), p_(p), t_(t), roster_(r), cap_(cap > MAX_ID ? MAX_ID : cap),
      goLiveDay_(goLiveDay), lastTick_(0), lastObservedNow_(0),
      clockRollbackSince_(0), badSince_(0), nextDueCheck_(0), processedThroughDay_(-1),
      badCause_(0), nextSessionToProcess_(0), bad_(false), pending_(false),
      indexOk_(false), progressReady_(false), clockRollback_(false),
      checkpointManaged_(false), checkpointAcknowledgedTick_(0) {
  const bool validInitialDays = goLiveDay_ >= 0 &&
      goLiveDay_ <= MAX_SUPPORTED_DAY && initialTodayDay >= goLiveDay_ &&
      initialTodayDay <= MAX_SUPPORTED_DAY;
  // Do not perform signed subtraction until the caller-provided day range has
  // been validated. This also avoids underflow for malformed constructor input.
  int32_t initialFirst = goLiveDay_;
  if (validInitialDays) {
    const int32_t rollingFirst = initialTodayDay >= MAX_TRACK_DAYS - 1
        ? initialTodayDay - (MAX_TRACK_DAYS - 1) : 0;
    if (rollingFirst > initialFirst) initialFirst = rollingFirst;
  }
  indexOk_ = validRosterCapacity(cap) && validInitialDays &&
      validClosureSchedule(p_, t_, goLiveDay_) &&
      index_.setWindow(initialFirst, initialTodayDay, p_.utcOffsetMin) &&
      log_.attach(&index_) && index_.evidenceHealthy();
}
bool ClosureEngine::matchesConfig(const Config& config) const {
  if (goLiveDay_ != config.goLiveDay ||
      p_.utcOffsetMin != config.params.utcOffsetMin ||
      p_.lateMin != config.params.lateMin ||
      p_.scanLeadMin != config.params.scanLeadMin) return false;
  for (int day = 0; day < 5; ++day) {
    for (int session = 0; session < 2; ++session) {
      const Slot& current = t_.day[day][session];
      const Slot& expected = config.tt.day[day][session];
      if (current.start != expected.start || current.end != expected.end ||
          current.enabled != expected.enabled) return false;
    }
  }
  return true;
}
void ClosureEngine::setSchedule(const Params& p, const Timetable& t) {
  p_ = p;
  t_ = t;
  nextSessionToProcess_ = 0;
  nextDueCheck_ = 0;
  progressReady_ = false;
  pending_ = false;
  if (!validClosureSchedule(p_, t_, goLiveDay_)) indexOk_ = false;
  pending_ = !indexOk_ || bad_ || !log_.readyToAppend();
}
// Only the part of [from,to) that touches scan windows can change coverage. Clip to the span from the first
// overlapped window start to the last overlapped window end. Nights and weekends with no window are dropped.
bool ClosureEngine::clipToWindows(uint32_t from, uint32_t to, uint32_t& cf, uint32_t& ct) const {
  if (to <= from || !validClosureSchedule(p_, t_, goLiveDay_)) return false;
  int32_t d0 = dayNumber(from, p_.utcOffsetMin);
  int32_t d1 = dayNumber(to - 1, p_.utcOffsetMin);
  if (d1 < goLiveDay_ || d0 > MAX_SUPPORTED_DAY) return false;
  if (d0 < goLiveDay_) d0 = goLiveDay_;
  if (d1 > MAX_SUPPORTED_DAY) d1 = MAX_SUPPORTED_DAY;
  if (d1 < d0) return false;
  // Timetable patterns repeat every seven local days. Search only the first
  // weekly cycle plus the partial first date, independent of outage duration.
  const int32_t spanDays = d1 - d0;
  const int32_t firstLimit = spanDays > 7 ? d0 + 7 : d1;
  bool firstFound = false;
  uint32_t first = 0;
  for (int32_t day = d0; day <= firstLimit && !firstFound; ++day) {
    const uint8_t weekday = weekdayIndex(day);
    if (weekday > 4) continue;
    const uint32_t ds = dayStartEpoch(day, p_.utcOffsetMin);
    for (int session = 0; session < 2; ++session) {
      const Slot& slot = t_.day[weekday][session];
      if (!slot.enabled) continue;
      const uint32_t ws = ds + (uint32_t)(slot.start - p_.scanLeadMin) * 60u;
      const uint32_t we = ds + (uint32_t)slot.end * 60u;
      if (ws < to && we > from) {
        first = ws > from ? ws : from;
        firstFound = true;
        break;
      }
    }
  }
  if (!firstFound) return false;

  // Mirror the search from the end. If the range exceeds seven day-boundaries,
  // the final eight dates contain a complete weekly cycle.
  const int32_t lastLimit = spanDays > 7 ? d1 - 7 : d0;
  bool lastFound = false;
  uint32_t last = 0;
  for (int32_t day = d1; day >= lastLimit && !lastFound; --day) {
    const uint8_t weekday = weekdayIndex(day);
    if (weekday > 4) continue;
    const uint32_t ds = dayStartEpoch(day, p_.utcOffsetMin);
    for (int session = 1; session >= 0; --session) {
      const Slot& slot = t_.day[weekday][session];
      if (!slot.enabled) continue;
      const uint32_t ws = ds + (uint32_t)(slot.start - p_.scanLeadMin) * 60u;
      const uint32_t we = ds + (uint32_t)slot.end * 60u;
      if (ws < to && we > from) {
        last = we < to ? we : to;
        lastFound = true;
        break;
      }
    }
  }
  if (!lastFound) return false;
  cf = first;
  ct = last;
  return true;
}
bool ClosureEngine::writeGap(uint32_t from, uint32_t to, uint16_t cause) {
  // clipToWindows() also returns false for invalid schedule state. Do not
  // confuse that failure with a valid outage that misses every scan window.
  if (!validClosureSchedule(p_, t_, goLiveDay_)) return false;
  if (to <= from) return true;

  // GAPs can be appended from boot, health recovery or a late heartbeat before
  // the next closure step. Advance the rolling projection window here first;
  // otherwise a day-401 GAP could be discarded as future data under a stale
  // day-1 window and would never be replayed into the projection later.
  const int32_t activeToday = dayNumber(to - 1u, p_.utcOffsetMin);
  if (activeToday < goLiveDay_ || activeToday > MAX_SUPPORTED_DAY) return false;
  const int32_t rollingFirst = activeToday >= MAX_TRACK_DAYS - 1
      ? activeToday - (MAX_TRACK_DAYS - 1) : 0;
  const int32_t first = goLiveDay_ > rollingFirst
      ? goLiveDay_ : rollingFirst;
  if (!index_.setWindow(first, activeToday, p_.utcOffsetMin)) {
    indexOk_ = false;
    pending_ = true;
    return false;
  }

  uint32_t cf = 0, ct = 0;
  if (!clipToWindows(from, to, cf, ct)) return true;     // valid schedule, no overlapping scan window
  return appendGaps(log_, cf, ct, cause);                // multi-group: never silently drops a long outage
}
bool ClosureEngine::observeTime_(uint32_t now) {
  if (lastObservedNow_ != 0 && now < lastObservedNow_) {
    if (!clockRollback_) {
      clockRollback_ = true;
      clockRollbackSince_ = lastObservedNow_;
    }
    if (!bad_) {
      bad_ = true;
      badSince_ = lastObservedNow_;
      badCause_ = GAP_POWER;
    }
    return false;
  }
  if (now > lastObservedNow_) lastObservedNow_ = now;
  return true;
}
void ClosureEngine::onBoot(uint32_t now, uint32_t lastTick) {
  if (lastTick != 0 && now < lastTick) {
    // A persisted tick newer than the RTC is evidence of clock rollback.
    // Preserve the cursor and hold closure until the clock catches up.
    clockRollback_ = true;
    clockRollbackSince_ = lastTick;
    if (!bad_) { bad_ = true; badSince_ = lastTick; badCause_ = GAP_POWER; }
    lastTick_ = lastTick;
    lastObservedNow_ = lastTick;
  } else {
    if (lastTick != 0 && now > lastTick && !writeGap(lastTick, now, GAP_POWER)) {
      bad_ = true; badSince_ = lastTick; badCause_ = GAP_POWER;
    }
    lastTick_ = now;
    lastObservedNow_ = now;
  }
  // Do not subtract from an untrusted anchor: malformed persisted/request
  // values may be INT32_MIN, and invalid engines still receive orderly boot
  // callbacks while the caller transitions to a fault state.
  processedThroughDay_ = (goLiveDay_ >= 0 && goLiveDay_ <= MAX_SUPPORTED_DAY)
      ? goLiveDay_ - 1 : -1;
  nextSessionToProcess_ = 0;
  nextDueCheck_ = 0;
  progressReady_ = false;
  pending_ = bad_ || !indexOk_ || !log_.readyToAppend();
}
void ClosureEngine::onHealth(uint32_t now, bool healthy, uint16_t cause) {
  if (!observeTime_(now)) return;
  if (!healthy) {
    if (!bad_) { bad_ = true; badSince_ = now; badCause_ = cause; }
    return;
  }
  // A zero-length interval cannot prove recovery from an observed clock rollback.
  if (bad_ && now > badSince_ &&
      (!clockRollback_ || now > clockRollbackSince_) &&
      writeGap(badSince_, now, badCause_)) {
    bad_ = false;
    clockRollback_ = false;
  }
}
bool ClosureEngine::onTick(uint32_t now) {
  if (!observeTime_(now)) return false;
  if (!indexOk_ || bad_ || !log_.readyToAppend()) return false;
  const int32_t day = dayNumber(now, p_.utcOffsetMin);
  if (day < goLiveDay_ || day > MAX_SUPPORTED_DAY) return false;

  // A final durable heartbeat at the scheduled session end is required before
  // that session can be snapshotted. Accept a short post-end grace so ordinary
  // loop jitter does not defer closure indefinitely; closeSession() still
  // requires lastTick_ >= the exact session end.
  bool inTickWindow = sessionAt(t_, p_, now) >= 0;
  if (!inTickWindow) {
    const uint8_t weekday = weekdayIndex(day);
    if (weekday <= 4) {
      const uint32_t dayStart = dayStartEpoch(day, p_.utcOffsetMin);
      for (int session = 0; session < 2; ++session) {
        const Slot& slot = t_.day[weekday][session];
        if (!slot.enabled || slot.end <= slot.start || slot.end > 1439)
          continue;
        const uint64_t end = static_cast<uint64_t>(dayStart) +
            static_cast<uint64_t>(slot.end) * 60u;
        if (end <= now && static_cast<uint64_t>(now) - end <=
                static_cast<uint64_t>(MAX_TICK_INTERVAL_S)) {
          inTickWindow = true;
          break;
        }
      }
    }
  }
  if (!inTickWindow) return false;

  if (lastTick_ != 0) {
    // UTC rollback must never move the closure tick cursor backwards. The
    // health/clock owner handles trust; this owner preserves monotonic progress.
    if (now < lastTick_) return false;
    if (now - lastTick_ < TICK_PERIOD_S) return false;
    // A late heartbeat is evidence that closure coverage was not continuously
    // observed. Persist the uncertain scan-window intersection before allowing
    // the checkpoint to move beyond it.
    if (now - lastTick_ > MAX_TICK_INTERVAL_S &&
        !writeGap(lastTick_, now, GAP_POWER)) {
      bad_ = true;
      badSince_ = lastTick_;
      badCause_ = GAP_POWER;
      pending_ = true;
      return false;
    }
  }
  lastTick_ = now;
  return true;
}
bool ClosureEngine::closeSession(int32_t day, int s, uint32_t now) {
  if (s < 0 || s >= 2 || day < goLiveDay_ || day > MAX_SUPPORTED_DAY) return false;
  const Slot& sl = t_.day[weekdayIndex(day)][s];
  if (!sl.enabled || sl.end <= sl.start || sl.end > 1439 ||
      sl.start < p_.scanLeadMin) return false;
  if (!log_.readyToAppend() || !roster_.historyComplete() ||
      index_.closeOverflow() || !index_.evidenceHealthy())
    return false;  // Never spend roster traversal on a group that cannot be committed.
  uint32_t ds = dayStartEpoch(day, p_.utcOffsetMin);
  uint32_t ws = ds + (uint32_t)(sl.start - p_.scanLeadMin) * 60;
  uint32_t we = ds + (uint32_t)sl.end * 60;
  // When a checkpoint coordinator owns this engine, CLOSE must never get
  // ahead of the durable coverage heartbeat. The coordinator acknowledges a
  // tick only after the NVS readback succeeds; an in-memory onTick() alone is
  // not sufficient to authorize a CLOSE append.
  if (checkpointManaged_ && checkpointAcknowledgedTick_ < we) return false;
  bool cov = !index_.gapOverflow() && !index_.gapOverlaps(ws, we);   // overflow = cannot prove coverage
  uint64_t words[5] = {0, 0, 0, 0, 0};
  uint16_t count = 0;
  for (uint16_t id = 1; id <= cap_; id++)
    if (roster_.expected(id, day)) { bitmapSet(words, id); count++; }
  // A projection may discover internal chain corruption during expected().
  // Never commit a partial CLOSE bitmap after that query revoked its health.
  if (!roster_.historyComplete()) return false;
  Event ev[MAX_GROUP];
  size_t n = 0;
  uint16_t pc = payloadCount(cap_);
  if (static_cast<size_t>(pc) + 1u > MAX_GROUP) return false;
  ev[n++] = Event{EV_CLOSE, makeFlags((uint8_t)s, 0, cov, false, false), count, now, 0, (uint16_t)day};
  for (uint16_t i = 0; i < pc; i++) ev[n++] = makePayload(words[i], 0, false);
  return log_.append(ev, n);
}
bool ClosureEngine::step(uint32_t now) {
  if (!observeTime_(now)) { pending_ = true; return false; }
  if (!indexOk_ || bad_ || !log_.readyToAppend()) { pending_ = true; return false; }
  const int32_t today = dayNumber(now, p_.utcOffsetMin);
  if (today < 0 || today > MAX_SUPPORTED_DAY) { pending_ = true; return false; }
  if (today < goLiveDay_) { pending_ = true; return false; }
  // Inclusive rolling retention: never track more than 400 local calendar days.
  // The immutable go-live day is only the initial anchor, never a scan-admission limit.
  const int32_t rollingFirst = today - (MAX_TRACK_DAYS - 1);
  const int32_t first = goLiveDay_ > rollingFirst ? goLiveDay_ : rollingFirst;
  const int32_t lastDay = today;
  if (!index_.setWindow(first, lastDay, p_.utcOffsetMin)) {
    indexOk_ = false;
    pending_ = true;
    return false;
  }

  // Rebase only when the rolling horizon/schedule moves past the current
  // cursor. Normal loop iterations resume at the exact day/session left off.
  if (!progressReady_ || processedThroughDay_ < first - 1 ||
      processedThroughDay_ > lastDay) {
    processedThroughDay_ = first - 1;
    nextSessionToProcess_ = 0;
    nextDueCheck_ = 0;
    progressReady_ = true;
  }
  if (nextDueCheck_ != 0 && now < nextDueCheck_) {
    pending_ = false;
    return true;
  }

  pending_ = false;
  int daysProcessed = 0;
  while (daysProcessed < MAX_CLOSURE_DAYS_PER_STEP) {
    const int32_t day = processedThroughDay_ + 1;
    if (day > lastDay) { pending_ = false; return true; }

    if (weekdayIndex(day) > 4) {
      processedThroughDay_ = day;
      nextSessionToProcess_ = 0;
      nextDueCheck_ = 0;
      ++daysProcessed;
      continue;
    }

    for (int session = nextSessionToProcess_; session < 2; ++session) {
      nextSessionToProcess_ = static_cast<uint8_t>(session);
      const Slot& slot = t_.day[weekdayIndex(day)][session];
      if (!slot.enabled || index_.closed(day, static_cast<uint8_t>(session))) {
        nextSessionToProcess_ = static_cast<uint8_t>(session + 1);
        continue;
      }

      const uint32_t closeAt = dayStartEpoch(day, p_.utcOffsetMin) +
          static_cast<uint32_t>(slot.end) * 60u;
      if (now < closeAt) {
        nextDueCheck_ = closeAt;
        pending_ = false;
        return true;
      }
      if (!closeSession(day, session, now)) {
        // Keep this cursor at the uncertain operation. After LogStore replay,
        // index_.closed() recognizes a complete-but-unacknowledged CLOSE and
        // prevents a duplicate append.
        pending_ = true;
        return false;
      }
      nextSessionToProcess_ = static_cast<uint8_t>(session + 1);
      nextDueCheck_ = 0;
    }

    processedThroughDay_ = day;
    nextSessionToProcess_ = 0;
    nextDueCheck_ = 0;
    ++daysProcessed;
  }

  // pending() reports remaining backlog after this bounded one-day slice.
  pending_ = processedThroughDay_ < lastDay;
  return true;
}
}
