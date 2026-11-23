#include "clock_confirmation.h"
#include "time_rules.h"

namespace att {
bool rtcSampleWithinWriteWindow(uint32_t requestedEpoch,
                                uint32_t observedEpoch,
                                uint64_t writeStartedUs,
                                uint64_t observedAtUs) {
  if (observedAtUs < writeStartedUs || observedEpoch < requestedEpoch)
    return false;
  const uint64_t elapsedUs = observedAtUs - writeStartedUs;
  const uint64_t maximumAdvanceSeconds = elapsedUs / 1000000ULL +
      (elapsedUs % 1000000ULL != 0 ? 1ULL : 0ULL);
  return static_cast<uint64_t>(observedEpoch - requestedEpoch) <=
      maximumAdvanceSeconds;
}
namespace {

const uint32_t kClockRequestEpochFloor = 1577836800UL;
const uint32_t kClockGapThresholdSeconds = 120u;

ClockConfirmResult result(ClockConfirmCode code, ClockAction action,
                          ClockApplyResult applied, uint32_t previousEpoch,
                          uint32_t requestedEpoch, uint32_t baseline,
                          bool trusted) {
  ClockConfirmResult value = {
      code, action, applied, previousEpoch, requestedEpoch, baseline, trusted};
  return value;
}

bool requestIdValid(const ClockConfirmRequest& request) {
  return request.requestId0 != 0 || request.requestId1 != 0 ||
      request.requestId2 != 0;
}

bool requestIdEqual(const uint16_t lhs[3], const ClockConfirmRequest& rhs) {
  return lhs[0] == rhs.requestId0 && lhs[1] == rhs.requestId1 &&
      lhs[2] == rhs.requestId2;
}

bool requestIdEqual(const uint16_t lhs[3], const uint16_t rhs[3]) {
  return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2];
}



struct ClockJournalScan {
  bool valid;
  bool pending;
  bool clockPairSeen;
  bool needsGap;
  bool targetIdSeen;
  bool targetIdCompleted;
  bool targetIdEpochMismatch;
  bool targetIdRepeated;
  uint16_t pendingId[3];
  uint16_t pendingSource;
  uint32_t pendingRequestedEpoch;
  uint32_t pendingOldEpoch;
  uint32_t gapCursor;
  uint32_t gapTarget;
  uint32_t targetOldEpoch;
  uint32_t targetRequestedEpoch;
};

bool journalIdMatches(const Event& first, const Event& second,
                      const ClockConfirmRequest& request) {
  const uint16_t id[3] = {first.a, first.b, second.a};
  return requestIdEqual(id, request);
}

bool journalIdMatches(const Event& first, const Event& second,
                      const uint16_t id[3]) {
  const uint16_t candidate[3] = {first.a, first.b, second.a};
  return requestIdEqual(candidate, id);
}

bool isJournalIntent(const Event* group, size_t count) {
  return count == 2 && group[0].type == EV_SETTING &&
      group[1].type == EV_SETTING &&
      group[0].flags == (FL_MORE | FL_SESSION) && group[1].flags == 0 &&
      group[1].b == 0 && group[0].epoch >= kClockRequestEpochFloor &&
      (group[1].epoch == 0 || group[1].epoch >= kClockRequestEpochFloor) &&
      (group[0].a != 0 || group[0].b != 0 || group[1].a != 0);
}

bool isJournalCompletion(const Event* group, size_t count) {
  return count == 2 && group[0].type == EV_SETTING &&
      group[1].type == EV_SETTING &&
      group[0].flags == (FL_MORE | FL_COVERAGE) && group[1].flags == 0 &&
      group[1].b == 0 && group[0].epoch >= kClockRequestEpochFloor &&
      group[1].epoch == group[0].epoch &&
      (group[0].a != 0 || group[0].b != 0 || group[1].a != 0);
}

bool clockPairMatches(const Event* group, size_t count, uint16_t source,
                      uint32_t oldEpoch, uint32_t newEpoch) {
  if (count < 2) return false;
  const Event& oldEvent = group[count - 2];
  const Event& newEvent = group[count - 1];
  return oldEvent.type == EV_CLOCK_SET && newEvent.type == EV_CLOCK_SET &&
      oldEvent.a == source && newEvent.a == source &&
      oldEvent.epoch == oldEpoch && newEvent.epoch == newEpoch &&
      oldEvent.b == 0 && newEvent.b == 0 &&
      oldEvent.flags == FL_MORE && newEvent.flags == 0 &&
      oldEvent.seq != UINT32_MAX && newEvent.seq == oldEvent.seq + 1u;
}

bool processPendingGroup(ClockJournalScan& scan, const Event* group,
                         size_t count) {
  if (scan.clockPairSeen) return false;
  const bool pairAtEnd = clockPairMatches(
      group, count, scan.pendingSource, scan.pendingOldEpoch,
      scan.pendingRequestedEpoch);
  const size_t prefixCount = pairAtEnd ? count - 2 : count;
  if (prefixCount > 0) {
    for (size_t i = 0; i < prefixCount; ++i) {
      const Event& event = group[i];
      if (event.type != EV_GAP || event.a != GAP_CLOCK || event.b == 0 ||
          !scan.needsGap || event.epoch != scan.gapCursor) return false;
      const uint64_t end = static_cast<uint64_t>(event.epoch) +
          static_cast<uint64_t>(event.b) * 60ULL;
      if (end > UINT32_MAX || end > static_cast<uint64_t>(scan.gapTarget) + 59ULL)
        return false;
      scan.gapCursor = static_cast<uint32_t>(end);
    }
  }
  if (pairAtEnd) {
    if (scan.needsGap &&
        (scan.gapCursor < scan.gapTarget ||
         scan.gapCursor - scan.gapTarget >= 60u)) return false;
    if (!scan.needsGap && prefixCount != 0) return false;
    scan.clockPairSeen = true;
    return true;
  }
  // GAP-only groups are emitted only while a representable prefix remains.
  return scan.needsGap && scan.gapCursor < scan.gapTarget;
}

bool scanClockJournal(LogStore& log, const ClockConfirmRequest& request,
                      ClockJournalScan& out) {
  ClockJournalScan scan = {};
  scan.valid = true;
  Event group[MAX_GROUP] = {};
  size_t groupCount = 0;

  for (size_t i = 0; i < log.count(); ++i) {
    Event event = {};
    if (!log.at(i, event) || groupCount >= MAX_GROUP) return false;
    group[groupCount++] = event;
    if (flagMore(event.flags)) continue;

    if (isJournalIntent(group, groupCount)) {
      if (scan.pending) return false;
      const uint16_t id[3] = {group[0].a, group[0].b, group[1].a};
      if (requestIdEqual(id, request)) {
        if (scan.targetIdSeen) scan.targetIdRepeated = true;
        scan.targetIdSeen = true;
        scan.targetOldEpoch = group[1].epoch;
        scan.targetRequestedEpoch = group[0].epoch;
        if (group[0].epoch != request.requestedUtcEpoch)
          scan.targetIdEpochMismatch = true;
      }
      scan.pending = true;
      scan.clockPairSeen = false;
      scan.pendingId[0] = id[0];
      scan.pendingId[1] = id[1];
      scan.pendingId[2] = id[2];
      scan.pendingRequestedEpoch = group[0].epoch;
      scan.pendingOldEpoch = group[1].epoch;
      scan.pendingSource = group[1].epoch == 0 ? SRC_WIZARD : SRC_MANUAL;
      const uint32_t delta = group[1].epoch > group[0].epoch
          ? group[1].epoch - group[0].epoch
          : group[0].epoch - group[1].epoch;
      scan.needsGap = delta > kClockGapThresholdSeconds;
      scan.gapCursor = scan.needsGap
          ? (group[1].epoch < group[0].epoch ? group[1].epoch : group[0].epoch)
          : 0;
      scan.gapTarget = scan.needsGap
          ? (group[1].epoch > group[0].epoch ? group[1].epoch : group[0].epoch)
          : 0;
    } else if (isJournalCompletion(group, groupCount)) {
      if (!scan.pending || !scan.clockPairSeen ||
          !journalIdMatches(group[0], group[1], scan.pendingId) ||
          group[0].epoch != scan.pendingRequestedEpoch ||
          group[1].epoch != scan.pendingRequestedEpoch) return false;
      if (journalIdMatches(group[0], group[1], request))
        scan.targetIdCompleted = true;
      scan.pending = false;
      scan.clockPairSeen = false;
    } else if (group[0].type == EV_SETTING) {
      // EV_SETTING has only the two exact journal shapes above. Do not treat
      // malformed or future settings records as unrelated application data.
      return false;
    } else if (scan.pending) {
      if (!processPendingGroup(scan, group, groupCount)) return false;
    }
    groupCount = 0;
  }
  if (groupCount != 0) return false;
  out = scan;
  return true;
}

void makeJournalEvents(const ClockConfirmRequest& request, uint32_t oldEpoch,
                       bool completion, uint32_t finalEpoch,
                       Event events[2]) {
  events[0] = Event{EV_SETTING,
                    static_cast<uint8_t>(completion ? FL_COVERAGE : FL_SESSION),
                    request.requestId0, request.requestedUtcEpoch, 0,
                    request.requestId1};
  events[1] = Event{EV_SETTING, 0, request.requestId2,
                    completion ? finalEpoch : oldEpoch, 0, 0};
}

bool appendJournal(LogStore& log, const ClockConfirmRequest& request,
                   uint32_t epoch, bool completion) {
  Event events[2] = {};
  makeJournalEvents(request, epoch, completion, epoch, events);
  return log.append(events, 2);
}

bool reopenAndScan(LogStore& log, const ClockConfirmRequest& request,
                   ClockJournalScan& scan) {
  const OpenInfo opened = log.open();
  return opened.ok && !log.faulted() &&
      scanClockJournal(log, request, scan);
}

ClockDecision resumedDecision(const ClockJournalScan& scan) {
  ClockDecision decision = {
      CA_APPLY, 0, scan.pendingSource, false, 0, 0};
  if (scan.needsGap && scan.gapCursor < scan.gapTarget) {
    decision.gapFrom = scan.gapCursor;
    decision.gapTo = scan.gapTarget;
  }
  return decision;
}

bool currentRtcProvesCompleted(const Ds3231Reading& reading,
                               const ClockJournalScan& scan,
                               uint32_t logBaseline) {
  return !reading.oscillatorStopped &&
      reading.epochUtc >= kClockRequestEpochFloor &&
      reading.epochUtc >= logBaseline &&
      reading.epochUtc >= scan.targetRequestedEpoch;
}

}  // namespace

ClockConfirmResult ClockConfirmationService::confirm(
    LogStore& log, MonthSet& months,
    const ClockConfirmRequest& request) {
  const ClockApplyResult notApplied = CLOCK_APPLY_REJECTED;
  if (blocked_)
    return result(CCF_SERVICE_LATCHED, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (!requestIdValid(request))
    return result(CCF_REQUEST_ID_INVALID, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);

  const HState healthState = health_.state();
  if (healthState != H_OK && healthState != H_CLOCK_FAULT &&
      healthState != H_SENSOR_FAULT)
    return result(CCF_HEALTH_BLOCKED, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  const bool currentClockFault = healthState == H_CLOCK_FAULT;

  const Config* config = configBoot_.config();
  if (!configBoot_.ready() || config == nullptr)
    return result(CCF_CONFIG_NOT_READY, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  const Params& params = config->params;
  const Timetable& timetable = config->tt;

  if (storageOwner_.state() != LITTLEFS_OWNER_READY)
    return result(CCF_STORAGE_OWNER_NOT_READY, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  IFileSet* ownerFiles = storageOwner_.fileSet();
  if (!ownerFiles)
    return result(CCF_STORAGE_OWNER_NOT_READY, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (!storageOwner_.writable())
    return result(CCF_STORAGE_OWNER_READ_ONLY, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (!months.usesFileSet(*ownerFiles))
    return result(CCF_STORAGE_OWNER_MISMATCH, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (log.faulted())
    return result(CCF_LOG_NOT_READY, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (!log.usesStorage(months))
    return result(CCF_STORAGE_MISMATCH, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);

  uint32_t requestedMonthKey = 0;
  if (!validateConfig(*config) ||
      request.requestedUtcEpoch < kClockRequestEpochFloor ||
      params.utcOffsetMin < -720 || params.utcOffsetMin > 840 ||
      !monthKeyForEpoch(request.requestedUtcEpoch, params.utcOffsetMin,
                        requestedMonthKey) ||
      !validateTimetable(timetable, params.scanLeadMin))
    return result(CCF_BAD_INPUT, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);

  ClockJournalScan journal = {};
  if (!scanClockJournal(log, request, journal)) {
    blocked_ = true;
    return result(CCF_LOG_REPLAY_FAILED, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  }
  if (journal.targetIdRepeated ||
      (journal.targetIdSeen && journal.targetIdEpochMismatch))
    return result(CCF_REQUEST_ID_REUSED, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);
  if (journal.pending && !requestIdEqual(journal.pendingId, request))
    return result(CCF_REQUEST_PENDING, CA_NONE, notApplied,
                  journal.pendingOldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);

  // A completed request can be replayed after a browser timeout or reboot.
  // It is a read-only receipt, and only current RTC/log agreement restores
  // clock trust; the old request never writes the RTC again.
  if (!journal.pending && journal.targetIdSeen &&
      journal.targetIdCompleted) {
    Ds3231Reading reading = {0, false};
    if (rtc_.readUtcEpoch(0, reading) != DS3231_OK ||
        !currentRtcProvesCompleted(reading, journal, log.baselineEpoch()))
      return result(CCF_REPLAY_CLOCK_UNTRUSTED, CA_NONE,
                    CLOCK_APPLY_REPLAYED, journal.targetOldEpoch,
                    request.requestedUtcEpoch, log.baselineEpoch(), false);
    return result(CCF_OK, CA_APPLY, CLOCK_APPLY_REPLAYED,
                  journal.targetOldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), true);
  }

  const bool resuming = journal.pending;
  uint32_t oldEpoch = 0;
  ClockDecision decision = {CA_NONE, 0, SRC_MANUAL, false, 0, 0};
  Ds3231Reading before = {0, false};
  const Ds3231Code readBefore = rtc_.readUtcEpoch(0, before);
  const bool rtcCalendarValid = readBefore == DS3231_OK;
  if (readBefore != DS3231_OK && readBefore != DS3231_INVALID_TIME)
    return result(CCF_RTC_READ_FAILED, CA_NONE, notApplied, 0,
                  request.requestedUtcEpoch, log.baselineEpoch(), false);

  if (resuming) {
    oldEpoch = journal.pendingOldEpoch;
    decision = resumedDecision(journal);
  } else {
    const uint32_t baseline = log.baselineEpoch();
    const bool belowLog = rtcCalendarValid && baseline != 0 &&
        before.epochUtc < baseline;
    const bool policyClockFault = currentClockFault || !rtcCalendarValid ||
        (rtcCalendarValid && (before.oscillatorStopped ||
         before.epochUtc < EPOCH_2020 || belowLog));
    const uint32_t deviceEpoch = rtcCalendarValid ? before.epochUtc : 0;

    bool inWindow = false;
    bool nearWindow = false;
    if (!policyClockFault) {
      inWindow = sessionAt(timetable, params, deviceEpoch) >= 0;
      const uint32_t margin = 300u;
      const uint32_t laterEpoch = deviceEpoch > UINT32_MAX - margin
          ? UINT32_MAX : deviceEpoch + margin;
      nearWindow = inWindow || sessionAt(timetable, params, laterEpoch) >= 0;
    }

    const ClockCtx context = {
        deviceEpoch, request.requestedUtcEpoch, baseline, 0,
        policyClockFault, true, request.confirmBackward, request.confirmJump,
        false, inWindow, nearWindow};
    decision = decideClock(context);
    oldEpoch = policyClockFault ? baseline : deviceEpoch;

    if (decision.action == CA_NEED_CONFIRM_BACK ||
        decision.action == CA_NEED_CONFIRM_JUMP)
      return result(CCF_CONFIRMATION_REQUIRED, decision.action, notApplied,
                    oldEpoch, request.requestedUtcEpoch, baseline, false);
    if (decision.action == CA_BLOCKED_WINDOW)
      return result(CCF_BLOCKED_WINDOW, decision.action, notApplied, oldEpoch,
                    request.requestedUtcEpoch, baseline, false);
    if (decision.action != CA_APPLY || decision.source > SRC_WIZARD ||
        !gapSpanEncodable(decision.gapFrom, decision.gapTo))
      return result(CCF_POLICY_REJECTED, decision.action, notApplied, oldEpoch,
                    request.requestedUtcEpoch, baseline, false);
  }

  months.setMonth(requestedMonthKey);

  // The first durable write records the exact request before any GAP/CLOCK_SET
  // data or RTC mutation. A retry can therefore distinguish a new operation
  // from continuation of the same logical operation.
  if (!resuming) {
    (void)appendJournal(log, request, oldEpoch, false);
    if (!reopenAndScan(log, request, journal) || !journal.pending ||
        !requestIdEqual(journal.pendingId, request)) {
      blocked_ = true;
      return result(CCF_LOG_REPLAY_FAILED, decision.action, notApplied,
                    oldEpoch, request.requestedUtcEpoch, log.baselineEpoch(),
                    false);
    }
    decision = resumedDecision(journal);
  }

  const bool clockPairExistedAtEntry = journal.clockPairSeen;
  if (!journal.clockPairSeen) {
    const ClockApplyResult applied =
        applyClockSet(log, decision, oldEpoch, request.requestedUtcEpoch);
    ClockJournalScan afterAppend = {};
    if (!reopenAndScan(log, request, afterAppend) || !afterAppend.pending ||
        !requestIdEqual(afterAppend.pendingId, request) ||
        !afterAppend.clockPairSeen) {
      blocked_ = true;
      return result(CCF_LOG_COMMIT_INCOMPLETE, decision.action, applied,
                    oldEpoch, request.requestedUtcEpoch, log.baselineEpoch(),
                    false);
    }
    journal = afterAppend;
  }

  // After a reboot, only a backward change with the RTC still strictly between
  // the requested value and the recorded pre-change value proves that the
  // write occurred. A forward RTC can naturally pass the requested value, so
  // that state is deliberately left uncertain rather than guessed complete.
  bool alreadyApplied = false;
  if (resuming && clockPairExistedAtEntry && journal.clockPairSeen && rtcCalendarValid &&
      !before.oscillatorStopped && request.requestedUtcEpoch < oldEpoch &&
      before.epochUtc >= request.requestedUtcEpoch &&
      before.epochUtc < oldEpoch) {
    alreadyApplied = true;
  } else if (resuming && clockPairExistedAtEntry && journal.clockPairSeen && rtcCalendarValid &&
             !before.oscillatorStopped &&
             before.epochUtc >= request.requestedUtcEpoch) {
    blocked_ = true;
    return result(CCF_REQUEST_RECONCILIATION_REQUIRED, decision.action,
                  CLOCK_APPLY_COMMITTED, oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }

  uint64_t writeStartedUs = 0;
  uint64_t afterWriteUs = 0;
  Ds3231Reading afterWrite = {0, false};
  ClockApplyResult appliedResult = CLOCK_APPLY_COMMITTED;
  if (!alreadyApplied) {
    if (!monotonic_.nowMicros(writeStartedUs)) {
      blocked_ = true;
      return result(CCF_MONOTONIC_FAILED, decision.action, appliedResult,
                    oldEpoch, request.requestedUtcEpoch,
                    log.baselineEpoch(), false);
    }
    const Ds3231Code writeCode =
        rtc_.writeUtcEpoch(request.requestedUtcEpoch, 0);
    if (writeCode == DS3231_IO_NOT_STARTED) {
      blocked_ = true;
      return result(CCF_RTC_WRITE_NOT_STARTED, decision.action, appliedResult,
                    oldEpoch, request.requestedUtcEpoch,
                    log.baselineEpoch(), false);
    }
    if (writeCode == DS3231_INVALID_ARGUMENT ||
        writeCode == DS3231_INVALID_TIME) {
      blocked_ = true;
      return result(CCF_RTC_WRITE_REJECTED, decision.action, appliedResult,
                    oldEpoch, request.requestedUtcEpoch,
                    log.baselineEpoch(), false);
    }

    const Ds3231Code readAfterWrite = rtc_.readUtcEpoch(0, afterWrite);
    if (readAfterWrite != DS3231_OK) {
      blocked_ = true;
      return result(writeCode == DS3231_IO_UNCERTAIN
                        ? CCF_RTC_WRITE_UNCERTAIN : CCF_RTC_VERIFY_FAILED,
                    decision.action, appliedResult, oldEpoch,
                    request.requestedUtcEpoch, log.baselineEpoch(), false);
    }
    if (!monotonic_.nowMicros(afterWriteUs) ||
        afterWriteUs < writeStartedUs) {
      blocked_ = true;
      return result(CCF_MONOTONIC_FAILED, decision.action, appliedResult,
                    oldEpoch, request.requestedUtcEpoch,
                    log.baselineEpoch(), false);
    }
    if (!rtcSampleWithinWriteWindow(request.requestedUtcEpoch,
                                    afterWrite.epochUtc,
                                    writeStartedUs, afterWriteUs)) {
      blocked_ = true;
      return result(writeCode == DS3231_IO_UNCERTAIN
                        ? CCF_RTC_WRITE_UNCERTAIN : CCF_RTC_VERIFY_FAILED,
                    decision.action, appliedResult, oldEpoch,
                    request.requestedUtcEpoch, log.baselineEpoch(), false);
    }
  } else {
    afterWrite = before;
    if (!monotonic_.nowMicros(afterWriteUs)) {
      blocked_ = true;
      return result(CCF_MONOTONIC_FAILED, decision.action, appliedResult,
                    oldEpoch, request.requestedUtcEpoch,
                    log.baselineEpoch(), false);
    }
  }

  // Clear OSF only after a calendar sample consistent with the requested UTC.
  // Re-read the calendar/status pair afterward to verify the flag transition.
  if (!alreadyApplied) (void)rtc_.clearOscillatorStopFlag();
  Ds3231Reading afterClear = {0, false};
  const Ds3231Code readAfterClear = rtc_.readUtcEpoch(0, afterClear);
  if (readAfterClear != DS3231_OK || afterClear.oscillatorStopped) {
    blocked_ = true;
    return result(CCF_OSF_CLEAR_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }
  uint64_t afterClearUs = 0;
  if (!monotonic_.nowMicros(afterClearUs) ||
      afterClearUs < afterWriteUs) {
    blocked_ = true;
    return result(CCF_MONOTONIC_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }
  if (!alreadyApplied &&
      !rtcSampleWithinWriteWindow(request.requestedUtcEpoch,
                                  afterClear.epochUtc,
                                  writeStartedUs, afterClearUs)) {
    blocked_ = true;
    return result(CCF_OSF_CLEAR_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }
  if (alreadyApplied && afterClear.epochUtc < request.requestedUtcEpoch) {
    blocked_ = true;
    return result(CCF_OSF_CLEAR_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }

  Ds3231Reading finalRtc = {0, false};
  if (rtc_.readUtcEpoch(0, finalRtc) != DS3231_OK ||
      finalRtc.oscillatorStopped || log.faulted() ||
      log.baselineEpoch() != request.requestedUtcEpoch) {
    blocked_ = true;
    return result(CCF_RTC_VERIFY_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch, log.baselineEpoch(),
                  false);
  }
  uint64_t finalReadUs = 0;
  if (!monotonic_.nowMicros(finalReadUs) ||
      finalReadUs < afterClearUs) {
    blocked_ = true;
    return result(CCF_MONOTONIC_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch, log.baselineEpoch(),
                  false);
  }
  if (!alreadyApplied &&
      !rtcSampleWithinWriteWindow(request.requestedUtcEpoch,
                                  finalRtc.epochUtc,
                                  writeStartedUs, finalReadUs)) {
    blocked_ = true;
    return result(CCF_RTC_VERIFY_FAILED, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch, log.baselineEpoch(),
                  false);
  }

  // A second durable marker closes the transaction only after full RTC
  // read-back, OSF-clear and log-baseline verification have succeeded.
  (void)appendJournal(log, request, request.requestedUtcEpoch, true);
  ClockJournalScan completed = {};
  if (!reopenAndScan(log, request, completed) || completed.pending ||
      !completed.targetIdSeen || !completed.targetIdCompleted ||
      log.baselineEpoch() != request.requestedUtcEpoch) {
    blocked_ = true;
    return result(CCF_LOG_COMMIT_INCOMPLETE, decision.action, appliedResult,
                  oldEpoch, request.requestedUtcEpoch,
                  log.baselineEpoch(), false);
  }

  return result(CCF_OK, decision.action, appliedResult, oldEpoch,
                request.requestedUtcEpoch, log.baselineEpoch(), true);
}

}  // namespace att
