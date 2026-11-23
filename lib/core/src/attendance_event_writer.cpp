#include "attendance_event_writer.h"

#include <string.h>

namespace att {
namespace {
bool validRequestId(const AttendanceMutationRequestId& id) {
  return id.words[0] != 0 || id.words[1] != 0 ||
      id.words[2] != 0 || id.words[3] != 0;
}
bool sameRequestId(const Event& marker,
                   const AttendanceMutationRequestId& id) {
  return marker.type == EV_ADMIN_MUTATION && marker.a == id.words[0] &&
      marker.b == id.words[1] &&
      marker.epoch == ((static_cast<uint32_t>(id.words[2]) << 16) |
                       static_cast<uint32_t>(id.words[3]));
}
bool sameOperation(const Event& stored, const Event& wanted) {
  // Epoch is intentionally omitted: an idempotent retry can arrive after a
  // reboot, and the caller's current RTC reading is not the original commit.
  return stored.type == wanted.type && stored.flags == wanted.flags &&
      stored.a == wanted.a && stored.b == wanted.b;
}
Event markSignature(const MarkReq& request) {
  return Event{EV_MARK, makeFlags(request.session, request.status, false, false,
                                  false), request.id, 0, 0, request.day};
}
Event todayStatusSignature(const TodayStatusMutationRequest& request) {
  return Event{EV_TODAY_STATUS,
      makeTodayStatusFlags(request.session, request.status),
      request.studentId, 0, 0, request.day};
}
bool sameTodayRequest(const TodayStatusMutationRequest& a,
                      const TodayStatusMutationRequest& b) {
  return a.studentId == b.studentId && a.day == b.day &&
      a.session == b.session && a.status == b.status &&
      a.expectedRevision == b.expectedRevision &&
      a.expectedRecords == b.expectedRecords &&
      a.generationHigh == b.generationHigh &&
      a.generationLow == b.generationLow;
}
bool sameTodayOperation(const Event& stored, const Event& intent) {
  if (stored.type != EV_TODAY_STATUS || intent.type != EV_TODAY_STATUS ||
      stored.a != intent.a || stored.b != intent.b ||
      flagSession(stored.flags) != flagSession(intent.flags)) return false;
  const uint8_t requested = flagStatus(intent.flags);
  const uint8_t committed = flagStatus(stored.flags);
  // The "present" choice is an intent. The terminal may durably resolve it
  // to Late after applying the configured cutoff. Exact retries replay that
  // committed outcome even if the RTC has since crossed the cutoff.
  if (requested == ST_PRESENT)
    return committed == ST_PRESENT || committed == ST_LATE;
  return committed == requested;
}
uint8_t todayCasFlags(uint8_t part) {
  return static_cast<uint8_t>(FL_MORE | (part << 1));
}
void encodeTodayCas(const TodayStatusMutationRequest& request, Event out[3]) {
  // The request is 22 bytes. Three Event payloads each preserve exactly 8
  // bytes (a, epoch, b); seq remains owned by LogStore.
  const uint32_t revisionAndRecords =
      (request.expectedRevision >> 16) |
      ((request.expectedRecords & 0xFFFFu) << 16);
  const uint32_t generationAndLow =
      (request.generationHigh >> 16) |
      ((request.generationLow & 0xFFFFu) << 16);
  out[0] = Event{EV_TODAY_CAS, todayCasFlags(0), request.studentId,
      static_cast<uint32_t>(request.day) |
          (static_cast<uint32_t>(request.session) << 16) |
          (static_cast<uint32_t>(request.status) << 24), 0, 0};
  out[1] = Event{EV_TODAY_CAS, todayCasFlags(1),
      static_cast<uint16_t>(request.expectedRevision), revisionAndRecords,
      0, static_cast<uint16_t>(request.expectedRecords >> 16)};
  out[2] = Event{EV_TODAY_CAS, todayCasFlags(2),
      static_cast<uint16_t>(request.generationHigh), generationAndLow,
      0, static_cast<uint16_t>(request.generationLow >> 16)};
}
Event autoNoClassReviewSignature(
    const AutomaticNoClassReviewRequest& request) {
  return Event{EV_AUTO_NOCLASS_REVIEW,
      static_cast<uint8_t>(request.session == 0 ? 0u : FL_SESSION),
      request.action, 0, 0, request.day};
}
bool sameAutoNoClassReviewCas(
    const Event cas[3], const AutomaticNoClassReviewRequest& request) {
  if (cas == nullptr) return false;
  const uint8_t firstFlags = static_cast<uint8_t>(FL_MORE |
      (request.session == 0 ? 0u : FL_SESSION));
  const uint64_t generation =
      (static_cast<uint64_t>(request.generationHigh) << 32) |
      static_cast<uint64_t>(request.generationLow);
  const uint32_t storedGenerationHigh = cas[1].epoch;
  const uint32_t storedGenerationLow = cas[2].epoch;
  const uint32_t storedRecords = static_cast<uint32_t>(cas[1].a) |
      (static_cast<uint32_t>(cas[1].b) << 16);
  const uint64_t storedGeneration =
      (static_cast<uint64_t>(storedGenerationHigh) << 32) |
      static_cast<uint64_t>(storedGenerationLow);
  return cas[0].type == EV_TODAY_CAS &&
      cas[0].a == AUTO_NOCLASS_REVIEW_CAS_MAGIC &&
      cas[0].flags == firstFlags &&
      cas[0].epoch == request.expectedAutoSequence && cas[0].b == request.day &&
      cas[1].type == EV_TODAY_CAS && cas[1].flags == (FL_MORE | 0x02u) &&
      cas[2].type == EV_TODAY_CAS && cas[2].flags == (FL_MORE | 0x04u) &&
      cas[2].a == 0 && cas[2].b == 0 &&
      storedRecords == request.expectedRecords && storedGeneration == generation;
}
bool sameTodayCas(const Event cas[3],
                  const TodayStatusMutationRequest& request) {
  if (cas == nullptr) return false;
  TodayStatusMutationRequest stored = {};
  stored.studentId = cas[0].a;
  stored.day = static_cast<uint16_t>(cas[0].epoch & 0xFFFFu);
  stored.session = static_cast<uint8_t>((cas[0].epoch >> 16) & 0xFFu);
  stored.status = static_cast<uint8_t>((cas[0].epoch >> 24) & 0xFFu);
  stored.expectedRevision = static_cast<uint32_t>(cas[1].a) |
      (static_cast<uint32_t>(cas[1].epoch & 0xFFFFu) << 16);
  stored.expectedRecords = (cas[1].epoch >> 16) |
      (static_cast<uint32_t>(cas[1].b) << 16);
  stored.generationHigh = static_cast<uint32_t>(cas[2].a) |
      ((cas[2].epoch & 0xFFFFu) << 16);
  stored.generationLow = (cas[2].epoch >> 16) |
      (static_cast<uint32_t>(cas[2].b) << 16);
  return cas[0].type == EV_TODAY_CAS && cas[0].flags == todayCasFlags(0) &&
      cas[0].b == 0 && cas[1].type == EV_TODAY_CAS &&
      cas[1].flags == todayCasFlags(1) && cas[2].type == EV_TODAY_CAS &&
      cas[2].flags == todayCasFlags(2) && sameTodayRequest(stored, request);
}
Event noClassSignature(const NoClassReq& request) {
  return Event{request.set ? EV_NOCLASS_SET : EV_NOCLASS_CLEAR,
      makeFlags(request.session, 0, false, false, request.wholeDay),
      0, 0, 0, request.day};
}
}  // namespace

AttendanceEventWriter::AttendanceEventWriter(
    LogStore& log, EventGroupAudit& audit,
    LogObserver* const* requiredObservers, size_t observerCount)
    : log_(log), audit_(audit), required_{nullptr, nullptr, nullptr, nullptr},
      requiredCount_(0), generation_(log.openGeneration()), faulted_(false),
      lookups_{}, legacyLookup_{} {
  if (requiredObservers == nullptr || observerCount == 0 ||
      observerCount > sizeof required_ / sizeof required_[0]) {
    faulted_ = true;
    return;
  }
  bool auditRequired = false;
  for (size_t i = 0; i < observerCount; ++i) {
    if (requiredObservers[i] == &audit_) auditRequired = true;
    if (requiredObservers[i] == nullptr) {
      faulted_ = true;
      return;
    }
    for (size_t j = 0; j < i; ++j) {
      if (requiredObservers[j] == requiredObservers[i]) {
        faulted_ = true;
        return;
      }
    }
    required_[i] = requiredObservers[i];
  }
  requiredCount_ = observerCount;
  if (!auditRequired) faulted_ = true;
}

AttendanceWriteOutcome AttendanceEventWriter::result_(
    AttendanceWriteState state, ApiCode code, ApiReason reason,
    uint32_t sequence) const {
  AttendanceWriteOutcome result = {state, code, reason, sequence};
  return result;
}

bool AttendanceEventWriter::ready_() {
  if (faulted_ || generation_ == 0 || log_.openGeneration() != generation_ ||
      log_.faulted() || requiredCount_ == 0 || !audit_.healthy() ||
      audit_.clockRequestPending())
    return false;
  if (!log_.hasObserver(&audit_)) {
    faulted_ = true;
    return false;
  }
  for (size_t i = 0; i < requiredCount_; ++i) {
    if (!log_.hasObserver(required_[i])) {
      faulted_ = true;
      return false;
    }
  }
  return log_.readyToAppend();
}

void AttendanceEventWriter::resetLookup_(
    LookupContext& lookup, const AttendanceMutationRequestId& requestId,
    const Event& signature, AdminMutationReason reason,
    uint64_t sessionFingerprint, uint32_t nowMonotonicS,
    const TodayStatusMutationRequest* todayRequest,
    const AutomaticNoClassReviewRequest* autoReviewRequest) {
  lookup.active = true;
  lookup.requestId = requestId;
  lookup.signature = signature;
  lookup.todayRequestValid = todayRequest != nullptr;
  if (todayRequest != nullptr) lookup.todayRequest = *todayRequest;
  else memset(&lookup.todayRequest, 0, sizeof lookup.todayRequest);
  lookup.autoReviewRequestValid = autoReviewRequest != nullptr;
  if (autoReviewRequest != nullptr)
    lookup.autoReviewRequest = *autoReviewRequest;
  else memset(&lookup.autoReviewRequest, 0, sizeof lookup.autoReviewRequest);
  lookup.reason = reason;
  lookup.sessionFingerprint = sessionFingerprint;
  lookup.cursor = 0;
  lookup.lastTouchedMonotonicS = nowMonotonicS;
}

AttendanceWriteOutcome AttendanceEventWriter::lookup_(
    const AttendanceMutationRequestId& requestId, const Event& signature,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    uint32_t nowMonotonicS, bool allowBloomMiss, bool& shouldAppend,
    uint64_t& appendSessionFingerprint,
    const TodayStatusMutationRequest* todayRequest,
    const AutomaticNoClassReviewRequest* autoReviewRequest) {
  shouldAppend = false;
  appendSessionFingerprint = 0;
  LookupContext* lookup = nullptr;
  // Expire and inspect every lane before allocating. In particular, IDs are
  // compared globally so a legacy retry cannot race a new audited request
  // using the same identity in the other pool.
  for (size_t i = 0; i < LOOKUP_CONTEXT_COUNT + 1u; ++i) {
    LookupContext& candidate = i < LOOKUP_CONTEXT_COUNT
        ? lookups_[i] : legacyLookup_;
    if (candidate.active &&
        static_cast<uint32_t>(nowMonotonicS - candidate.lastTouchedMonotonicS) >
            LOOKUP_CONTEXT_IDLE_TIMEOUT_S)
      candidate.active = false;  // retry safely restarts its durable log scan
    if (!candidate.active) continue;
    const bool sameId =
        candidate.requestId.words[0] == requestId.words[0] &&
        candidate.requestId.words[1] == requestId.words[1] &&
        candidate.requestId.words[2] == requestId.words[2] &&
        candidate.requestId.words[3] == requestId.words[3];
    if (!sameId) continue;
    // Do not let two different operations/reasons sharing one pending
    // identity reset each other's scan cursor. The first request owns it.
    if (!sameOperation(candidate.signature, signature) ||
        candidate.reason != reason ||
        (signature.type == EV_TODAY_STATUS &&
         (!candidate.todayRequestValid || todayRequest == nullptr ||
          !sameTodayRequest(candidate.todayRequest, *todayRequest))) ||
        (signature.type == EV_AUTO_NOCLASS_REVIEW &&
         (!candidate.autoReviewRequestValid || autoReviewRequest == nullptr ||
              candidate.autoReviewRequest.day != autoReviewRequest->day ||
          candidate.autoReviewRequest.session != autoReviewRequest->session ||
          candidate.autoReviewRequest.action != autoReviewRequest->action ||
          candidate.autoReviewRequest.expectedAutoSequence !=
              autoReviewRequest->expectedAutoSequence ||
          candidate.autoReviewRequest.expectedRecords !=
              autoReviewRequest->expectedRecords ||
          candidate.autoReviewRequest.generationHigh !=
              autoReviewRequest->generationHigh ||
          candidate.autoReviewRequest.generationLow !=
              autoReviewRequest->generationLow)))
      return result_(AWS_ID_REUSED, A_CONFLICT, R_STATE);
    candidate.lastTouchedMonotonicS = nowMonotonicS;
    lookup = &candidate;
    break;
  }

  if (allowBloomMiss && lookup == nullptr &&
      !audit_.mayContainAdminMutation(requestId.words)) {
    // The validated replay-built Bloom filter proves this identity is absent.
    // Avoid a full linear flash scan for every new item in a large batch.
    appendSessionFingerprint = sessionFingerprint;
    shouldAppend = true;
    return result_(AWS_BLOCKED, A_OK, R_NONE);
  }

  if (lookup == nullptr) {
    if (reason == AMR_NONE && signature.type != EV_TODAY_STATUS &&
        signature.type != EV_AUTO_NOCLASS_REVIEW) {
      // Compatibility lookups exist only to replay a previously committed
      // legacy pair. Reasonless Today events still carry a full audit record
      // and therefore use the bounded audited lookup lanes.
      if (legacyLookup_.active)
        return result_(AWS_LOOKUP_PENDING, A_CONFLICT, R_STATE);
      lookup = &legacyLookup_;
    } else {
      for (size_t i = 0; i < LOOKUP_CONTEXT_COUNT; ++i) {
        if (!lookups_[i].active) {
          lookup = &lookups_[i];
          break;
        }
      }
      // Never evict an in-progress identity scan. A fifth audited request is
      // back-pressured until one of the four durable-history scans completes.
      if (lookup == nullptr)
        return result_(AWS_LOOKUP_PENDING, A_CONFLICT, R_STATE);
    }
    resetLookup_(*lookup, requestId, signature, reason,
                 sessionFingerprint, nowMonotonicS, todayRequest,
                 autoReviewRequest);
  }

  const size_t recordCount = log_.count();
  size_t scanned = 0;
  while (lookup->cursor < recordCount && scanned < LOOKUP_RECORD_BUDGET) {
    Event current = {};
    if (!log_.at(lookup->cursor, current)) {
      faulted_ = true;
      log_.requireReopen();
      lookup->active = false;
      return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
    }
    ++scanned;
    if (current.type == EV_ADMIN_AUDIT || current.type == EV_TODAY_CAS) {
      // This record must be consumed together with its preceding marker. A
      // cursor reaching it independently means the log/group contract drifted.
      faulted_ = true;
      log_.requireReopen();
      lookup->active = false;
      return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
    }
    if (current.type != EV_ADMIN_MUTATION) {
      ++lookup->cursor;
      continue;
    }

    // Identity groups may be legacy pairs, historical audit triples, or
    // six-record Today commits. Never advance beyond a marker unless the
    // complete group is available; finish a group even at a page boundary.
    if (lookup->cursor + 1 >= recordCount) {
      faulted_ = true;
      log_.requireReopen();
      lookup->active = false;
      return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
    }
    Event second = {};
    if (!log_.at(lookup->cursor + 1, second)) {
      faulted_ = true;
      log_.requireReopen();
      lookup->active = false;
      return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
    }
    ++scanned;
    const bool hasAudit = second.type == EV_ADMIN_AUDIT;
    size_t groupRecords = hasAudit ? 3u : 2u;
    Event operation = second;
    Event audit = {};
    Event cas[3] = {};
    Event autoReviewCas[3] = {};
    bool hasTodayCas = false;
    bool hasAutoReviewCas = false;
    if (hasAudit) {
      if (lookup->cursor + 2 >= recordCount) {
        faulted_ = true;
        log_.requireReopen();
        lookup->active = false;
        return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
      }
      Event third = {};
      if (!log_.at(lookup->cursor + 2, third)) {
        faulted_ = true;
        log_.requireReopen();
        lookup->active = false;
        return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
      }
      ++scanned;
      audit = second;
      if (third.type == EV_TODAY_CAS &&
          third.a == AUTO_NOCLASS_REVIEW_CAS_MAGIC) {
        hasAutoReviewCas = true;
        autoReviewCas[0] = third;
        groupRecords = 6u;
        if (lookup->cursor + 5u >= recordCount ||
            !log_.at(lookup->cursor + 3u, autoReviewCas[1]) ||
            !log_.at(lookup->cursor + 4u, autoReviewCas[2]) ||
            !log_.at(lookup->cursor + 5u, operation)) {
          faulted_ = true;
          log_.requireReopen();
          lookup->active = false;
          return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
        }
        scanned += 3u;
      } else if (third.type == EV_TODAY_CAS) {
        hasTodayCas = true;
        cas[0] = third;
        groupRecords = 6u;
        if (lookup->cursor + 5 >= recordCount ||
            !log_.at(lookup->cursor + 3, cas[1]) ||
            !log_.at(lookup->cursor + 4, cas[2]) ||
            !log_.at(lookup->cursor + 5, operation)) {
          faulted_ = true;
          log_.requireReopen();
          lookup->active = false;
          return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
        }
        scanned += 3u;
      } else {
        operation = third;
      }
    }

    lookup->cursor += groupRecords;
    if (sameRequestId(current, requestId)) {
      lookup->active = false;
      const bool auditMatches = hasAudit
          ? (audit.a == static_cast<uint16_t>(reason))
          : (reason == AMR_NONE && signature.type != EV_TODAY_STATUS &&
             signature.type != EV_AUTO_NOCLASS_REVIEW);
      const bool casMatches = signature.type == EV_TODAY_STATUS
          ? (hasTodayCas && !hasAutoReviewCas && todayRequest != nullptr &&
             sameTodayCas(cas, *todayRequest))
          : signature.type == EV_AUTO_NOCLASS_REVIEW
              ? (hasAutoReviewCas && !hasTodayCas && autoReviewRequest != nullptr &&
                 sameAutoNoClassReviewCas(autoReviewCas, *autoReviewRequest))
              : !hasTodayCas && !hasAutoReviewCas;
      const bool operationMatches = signature.type == EV_TODAY_STATUS
          ? sameTodayOperation(operation, signature)
          : sameOperation(operation, signature);
      if (!auditMatches || !casMatches || !operationMatches)
        return result_(AWS_ID_REUSED, A_CONFLICT, R_STATE);
      return result_(AWS_REPLAYED, A_OK, R_NONE, operation.seq);
    }
  }

  if (lookup->cursor < recordCount)
    return result_(AWS_LOOKUP_PENDING, A_CONFLICT, R_STATE);

  appendSessionFingerprint = lookup->sessionFingerprint;
  lookup->active = false;
  shouldAppend = true;
  return result_(AWS_BLOCKED, A_OK, R_NONE);  // internal no-match sentinel
}

AttendanceWriteOutcome AttendanceEventWriter::mark(
    const MarkReq& request, const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const ApiCtx& context, const RosterStore& roster) {
  return mark_(request, requestId, reason, sessionFingerprint, context, roster,
               false);
}

AttendanceWriteOutcome AttendanceEventWriter::markHistorical(
    const MarkReq& request, const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const ApiCtx& context, const RosterStore& roster) {
  return mark_(request, requestId, reason, sessionFingerprint, context, roster,
               true);
}

AttendanceWriteOutcome AttendanceEventWriter::mark_(
    const MarkReq& request, const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const ApiCtx& context, const RosterStore& roster, bool historicalOnly) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (!validRequestId(requestId))
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const Event signature = markSignature(request);
  bool shouldAppend = false;
  uint64_t appendSessionFingerprint = 0;
  AttendanceWriteOutcome found = lookup_(requestId, signature, reason,
      sessionFingerprint, context.monotonicNow, false, shouldAppend,
      appendSessionFingerprint);
  if (!shouldAppend) return found;  // exact committed retry takes precedence
  if (reason < AMR_DATA_ENTRY_ERROR || reason > AMR_OTHER_DOCUMENTED ||
      appendSessionFingerprint == 0 ||
      appendSessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  if (historicalOnly && request.day ==
      dayNumber(context.now, context.p.utcOffsetMin))
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);

  Event operation = {};
  size_t count = 0;
  const ApiResult planned = buildMark(request, context, roster, &operation, count);
  if (planned.code != A_OK || count != 1)
    return result_(AWS_REJECTED, planned.code, planned.reason);
  return append_(requestId, reason, appendSessionFingerprint, operation);
}

AttendanceWriteOutcome AttendanceEventWriter::markIfSnapshot(
    const MarkReq& request, const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const ApiCtx& context, const RosterStore& roster,
    bool historicalCellEligible, uint32_t expectedRecords,
    uint64_t expectedGeneration) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (!validRequestId(requestId))
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const Event signature = markSignature(request);
  bool shouldAppend = false;
  uint64_t appendSessionFingerprint = 0;
  AttendanceWriteOutcome found = lookup_(requestId, signature, reason,
      sessionFingerprint, context.monotonicNow, true, shouldAppend,
      appendSessionFingerprint);
  if (!shouldAppend) return found;  // exact committed retry takes precedence
  if (reason < AMR_DATA_ENTRY_ERROR || reason > AMR_OTHER_DOCUMENTED ||
      appendSessionFingerprint == 0 ||
      appendSessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  // Exact request replay deliberately precedes historical eligibility and
  // snapshot checks: a response may have been lost after commit, followed by
  // a reboot or later events. A genuinely new request must pass both gates.
  if (!historicalCellEligible)
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);

  // No writes occur inside lookup_(). This exact comparison therefore forms
  // the CAS boundary immediately before validation and append. Any scan,
  // closure, roster change, or other correction since the grid was loaded
  // makes the browser reload instead of applying stale cell decisions.
  if (expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
      expectedGeneration == 0 ||
      log_.openGeneration() != expectedGeneration ||
      generation_ != expectedGeneration ||
      log_.count() != static_cast<size_t>(expectedRecords))
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);

  Event operation = {};
  size_t count = 0;
  const ApiResult planned = buildMark(request, context, roster, &operation, count);
  if (planned.code != A_OK || count != 1)
    return result_(AWS_REJECTED, planned.code, planned.reason);
  return append_(requestId, reason, appendSessionFingerprint, operation);
}

AttendanceWriteOutcome AttendanceEventWriter::todayStatus(
    const TodayStatusMutationRequest& request,
    const AttendanceMutationRequestId& requestId,
    uint64_t sessionFingerprint, const ApiCtx& context,
    RosterStore& roster, bool eligible, uint32_t currentRevision,
    uint8_t effectiveStatus) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (effectiveStatus == 0) effectiveStatus = request.status;
  const bool validEffectiveStatus =
      (request.status == ST_PRESENT &&
       (effectiveStatus == ST_PRESENT || effectiveStatus == ST_LATE)) ||
      (request.status == ST_EXCUSED && effectiveStatus == ST_EXCUSED) ||
      (request.status == ST_PENDING && effectiveStatus == ST_PENDING);
  if (!validRequestId(requestId) || request.studentId < 1 ||
      request.studentId > MAX_ID || request.session > 1 ||
      (request.status != ST_PRESENT && request.status != ST_PENDING &&
       request.status != ST_EXCUSED) || !validEffectiveStatus ||
      sessionFingerprint == 0 || sessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const Event signature = todayStatusSignature(request);
  bool shouldAppend = false;
  uint64_t appendSessionFingerprint = 0;
  AttendanceWriteOutcome found = lookup_(requestId, signature, AMR_NONE,
      sessionFingerprint, context.monotonicNow, true, shouldAppend,
      appendSessionFingerprint, &request);
  if (!shouldAppend) return found;  // durable retry precedes all mutable gates

  if (!eligible || !roster.historyComplete() || !roster.healthy() ||
      !roster.known(request.studentId) ||
      !roster.expected(request.studentId, request.day))
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);
  if (request.expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
      (request.generationHigh == 0 && request.generationLow == 0))
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);
  const uint64_t expectedGeneration =
      (static_cast<uint64_t>(request.generationHigh) << 32) |
      static_cast<uint64_t>(request.generationLow);
  // Compare-and-set is immediately adjacent to append. Exact retries were
  // already resolved above, even if the global log has since advanced.
  if (expectedGeneration == 0 || expectedGeneration != generation_ ||
      expectedGeneration != log_.openGeneration() ||
      log_.count() != static_cast<size_t>(request.expectedRecords) ||
      currentRevision != request.expectedRevision)
    return result_(AWS_REJECTED, A_CONFLICT, R_GEN_MISMATCH);

  const int32_t trustedDay = dayNumber(context.now, context.p.utcOffsetMin);
  if (!apiDaySupported(trustedDay) || trustedDay != request.day ||
      weekdayIndex(trustedDay) > 4 ||
      !context.tt.day[weekdayIndex(trustedDay)][request.session].enabled)
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);

  Event operation = {EV_TODAY_STATUS,
      makeTodayStatusFlags(request.session, effectiveStatus),
      request.studentId, context.now, 0, request.day};
  if (flagMore(operation.flags) || operation.epoch == 0)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);
  Event cas[3] = {};
  encodeTodayCas(request, cas);
  return appendToday_(requestId, appendSessionFingerprint, cas, operation);
}

AttendanceWriteOutcome AttendanceEventWriter::noClass(
    const NoClassReq& request, const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const ApiCtx& context) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (!validRequestId(requestId))
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const Event signature = noClassSignature(request);
  bool shouldAppend = false;
  uint64_t appendSessionFingerprint = 0;
  AttendanceWriteOutcome found = lookup_(requestId, signature, reason,
      sessionFingerprint, context.monotonicNow, false, shouldAppend,
      appendSessionFingerprint);
  if (!shouldAppend) return found;
  if (reason < AMR_DATA_ENTRY_ERROR || reason > AMR_OTHER_DOCUMENTED ||
      appendSessionFingerprint == 0 ||
      appendSessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  Event operation = {};
  size_t count = 0;
  const ApiResult planned = buildNoClass(request, context, &operation, count);
  if (planned.code != A_OK || count != 1)
    return result_(AWS_REJECTED, planned.code, planned.reason);
  return append_(requestId, reason, appendSessionFingerprint, operation);
}

AttendanceWriteOutcome AttendanceEventWriter::reviewAutomaticNoClass(
    const AutomaticNoClassReviewRequest& request,
    const AttendanceMutationRequestId& requestId,
    uint64_t sessionFingerprint, const ApiCtx& context, bool eligible) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (!validRequestId(requestId) || request.day > MAX_SUPPORTED_DAY ||
      request.session > 1 ||
      (request.action != ANCR_CONFIRM && request.action != ANCR_UNDO) ||
      request.expectedAutoSequence == 0 || sessionFingerprint == 0 ||
      sessionFingerprint > 0xFFFFFFFFFFFFULL || context.now < EPOCH_2020)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const Event signature = autoNoClassReviewSignature(request);
  bool shouldAppend = false;
  uint64_t appendSessionFingerprint = 0;
  AttendanceWriteOutcome found = lookup_(requestId, signature, AMR_NONE,
      sessionFingerprint, context.monotonicNow, true, shouldAppend,
      appendSessionFingerprint, nullptr, &request);
  if (!shouldAppend) return found;  // exact retry is resolved before state/CAS gates

  if (appendSessionFingerprint == 0 ||
      appendSessionFingerprint > 0xFFFFFFFFFFFFULL || !eligible ||
      request.expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
      (request.generationHigh == 0 && request.generationLow == 0))
    return result_(AWS_REJECTED, A_CONFLICT, R_STATE);
  const uint64_t expectedGeneration =
      (static_cast<uint64_t>(request.generationHigh) << 32) |
      static_cast<uint64_t>(request.generationLow);
  if (expectedGeneration == 0 || expectedGeneration != generation_ ||
      expectedGeneration != log_.openGeneration() ||
      log_.count() != static_cast<size_t>(request.expectedRecords) ||
      request.expectedAutoSequence > log_.lastSeq())
    return result_(AWS_REJECTED, A_CONFLICT, R_GEN_MISMATCH);

  Event cas[3] = {
      Event{EV_TODAY_CAS,
          static_cast<uint8_t>(FL_MORE |
              (request.session == 0 ? 0u : FL_SESSION)),
          AUTO_NOCLASS_REVIEW_CAS_MAGIC, request.expectedAutoSequence, 0,
          request.day},
      Event{EV_TODAY_CAS, static_cast<uint8_t>(FL_MORE | 0x02u),
          static_cast<uint16_t>(request.expectedRecords & 0xFFFFu),
          request.generationHigh, 0,
          static_cast<uint16_t>(request.expectedRecords >> 16)},
      Event{EV_TODAY_CAS, static_cast<uint8_t>(FL_MORE | 0x04u), 0,
          request.generationLow, 0, 0}};
  Event operation = Event{EV_AUTO_NOCLASS_REVIEW,
      static_cast<uint8_t>(request.session == 0 ? 0u : FL_SESSION), request.action,
      context.now, 0, request.day};
  return appendAutoNoClassReview_(requestId, appendSessionFingerprint,
                                  cas, operation);
}

AttendanceWriteOutcome AttendanceEventWriter::appendAutoNoClassReview_(
    const AttendanceMutationRequestId& requestId,
    uint64_t sessionFingerprint, const Event casIdentity[3],
    const Event& operation) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (casIdentity == nullptr || casIdentity[0].type != EV_TODAY_CAS ||
      casIdentity[0].a != AUTO_NOCLASS_REVIEW_CAS_MAGIC ||
      (casIdentity[0].flags != FL_MORE &&
       casIdentity[0].flags != (FL_MORE | FL_SESSION)) ||
      casIdentity[0].epoch == 0 ||
      casIdentity[1].type != EV_TODAY_CAS ||
      casIdentity[1].flags != (FL_MORE | 0x02u) ||
      casIdentity[2].type != EV_TODAY_CAS ||
      casIdentity[2].flags != (FL_MORE | 0x04u) ||
      casIdentity[2].a != 0 || casIdentity[2].b != 0 ||
      operation.type != EV_AUTO_NOCLASS_REVIEW ||
      operation.epoch == 0 || operation.b != casIdentity[0].b ||
      flagSession(operation.flags) != flagSession(casIdentity[0].flags) ||
      (operation.a != ANCR_CONFIRM && operation.a != ANCR_UNDO) ||
      sessionFingerprint == 0 || sessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);

  const size_t beforeCount = log_.count();
  const uint32_t beforeSeq = log_.lastSeq();
  const uint64_t generation = log_.openGeneration();
  if (beforeCount > static_cast<size_t>(-1) - 6u ||
      beforeSeq > UINT32_MAX - 7u)
    return result_(AWS_BLOCKED, A_LOCKED, R_FULL);
  Event group[6] = {
      Event{EV_ADMIN_MUTATION, FL_MORE, requestId.words[0],
            (static_cast<uint32_t>(requestId.words[2]) << 16) |
                static_cast<uint32_t>(requestId.words[3]),
            0, requestId.words[1]},
      Event{EV_ADMIN_AUDIT, FL_MORE, AMR_NONE,
            static_cast<uint32_t>(sessionFingerprint & 0xFFFFFFFFu),
            0, static_cast<uint16_t>(sessionFingerprint >> 32)},
      casIdentity[0], casIdentity[1], casIdentity[2], operation};
  if (!log_.append(group, 6)) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }
  Event stored[6] = {};
  bool exact = true;
  for (size_t i = 0; i < 6; ++i)
    if (!log_.at(beforeCount + i, stored[i])) exact = false;
  exact = exact && stored[0].seq == beforeSeq + 1u &&
      stored[0].type == EV_ADMIN_MUTATION && stored[0].flags == FL_MORE &&
      sameRequestId(stored[0], requestId) &&
      stored[1].seq == beforeSeq + 2u &&
      stored[1].type == EV_ADMIN_AUDIT && stored[1].flags == FL_MORE &&
      stored[1].a == AMR_NONE &&
      (static_cast<uint64_t>(stored[1].epoch) |
       (static_cast<uint64_t>(stored[1].b) << 32)) == sessionFingerprint &&
      stored[2].seq == beforeSeq + 3u && stored[2].type == EV_TODAY_CAS &&
      stored[2].flags == casIdentity[0].flags &&
      stored[2].a == casIdentity[0].a && stored[2].epoch == casIdentity[0].epoch &&
      stored[2].b == casIdentity[0].b &&
      stored[3].seq == beforeSeq + 4u && stored[3].type == EV_TODAY_CAS &&
      stored[3].flags == casIdentity[1].flags &&
      stored[3].a == casIdentity[1].a && stored[3].epoch == casIdentity[1].epoch &&
      stored[3].b == casIdentity[1].b &&
      stored[4].seq == beforeSeq + 5u && stored[4].type == EV_TODAY_CAS &&
      stored[4].flags == casIdentity[2].flags &&
      stored[4].a == casIdentity[2].a && stored[4].epoch == casIdentity[2].epoch &&
      stored[4].b == casIdentity[2].b &&
      stored[5].seq == beforeSeq + 6u &&
      sameOperation(stored[5], operation) && stored[5].epoch == operation.epoch &&
      !flagMore(stored[5].flags);
  const bool postcondition = exact && !log_.faulted() &&
      log_.openGeneration() == generation && generation == generation_ &&
      log_.count() == beforeCount + 6u && log_.lastSeq() == beforeSeq + 6u;
  if (!postcondition) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }
  return result_(AWS_COMMITTED, A_OK, R_NONE, stored[5].seq);
}

AttendanceWriteOutcome AttendanceEventWriter::appendToday_(
    const AttendanceMutationRequestId& requestId,
    uint64_t sessionFingerprint, const Event casIdentity[3],
    const Event& operation) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (operation.type != EV_TODAY_STATUS || casIdentity == nullptr ||
      casIdentity[0].type != EV_TODAY_CAS ||
      casIdentity[0].flags != todayCasFlags(0) ||
      casIdentity[1].type != EV_TODAY_CAS ||
      casIdentity[1].flags != todayCasFlags(1) ||
      casIdentity[2].type != EV_TODAY_CAS ||
      casIdentity[2].flags != todayCasFlags(2) ||
      sessionFingerprint == 0 || sessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);
  const size_t beforeCount = log_.count();
  const uint32_t beforeSeq = log_.lastSeq();
  const uint64_t generation = log_.openGeneration();
  if (beforeCount > static_cast<size_t>(-1) - 6u ||
      beforeSeq > UINT32_MAX - 7u)
    return result_(AWS_BLOCKED, A_LOCKED, R_FULL);
  Event group[6] = {
      Event{EV_ADMIN_MUTATION, FL_MORE, requestId.words[0],
            (static_cast<uint32_t>(requestId.words[2]) << 16) |
                static_cast<uint32_t>(requestId.words[3]),
            0, requestId.words[1]},
      Event{EV_ADMIN_AUDIT, FL_MORE, AMR_NONE,
            static_cast<uint32_t>(sessionFingerprint & 0xFFFFFFFFu),
            0, static_cast<uint16_t>(sessionFingerprint >> 32)},
      casIdentity[0], casIdentity[1], casIdentity[2], operation};
  if (!log_.append(group, 6)) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }
  Event stored[6] = {};
  bool exact = true;
  for (size_t i = 0; i < 6; ++i)
    if (!log_.at(beforeCount + i, stored[i])) exact = false;
  exact = exact && stored[0].seq == beforeSeq + 1u &&
      stored[0].type == EV_ADMIN_MUTATION && stored[0].flags == FL_MORE &&
      sameRequestId(stored[0], requestId) &&
      stored[1].seq == beforeSeq + 2u &&
      stored[1].type == EV_ADMIN_AUDIT && stored[1].flags == FL_MORE &&
      stored[1].a == AMR_NONE &&
      (static_cast<uint64_t>(stored[1].epoch) |
       (static_cast<uint64_t>(stored[1].b) << 32)) == sessionFingerprint &&
      stored[2].seq == beforeSeq + 3u &&
      stored[2].type == EV_TODAY_CAS && stored[2].flags == todayCasFlags(0) &&
      stored[2].a == casIdentity[0].a && stored[2].epoch == casIdentity[0].epoch &&
      stored[2].b == casIdentity[0].b &&
      stored[3].seq == beforeSeq + 4u &&
      stored[3].type == EV_TODAY_CAS && stored[3].flags == todayCasFlags(1) &&
      stored[3].a == casIdentity[1].a && stored[3].epoch == casIdentity[1].epoch &&
      stored[3].b == casIdentity[1].b &&
      stored[4].seq == beforeSeq + 5u &&
      stored[4].type == EV_TODAY_CAS && stored[4].flags == todayCasFlags(2) &&
      stored[4].a == casIdentity[2].a && stored[4].epoch == casIdentity[2].epoch &&
      stored[4].b == casIdentity[2].b &&
      stored[5].seq == beforeSeq + 6u && sameOperation(stored[5], operation) &&
      stored[5].epoch == operation.epoch && !flagMore(stored[5].flags);
  const bool postcondition = exact && !log_.faulted() &&
      log_.openGeneration() == generation && generation == generation_ &&
      log_.count() == beforeCount + 6u && log_.lastSeq() == beforeSeq + 6u;
  if (!postcondition) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }
  return result_(AWS_COMMITTED, A_OK, R_NONE, stored[5].seq);
}

AttendanceWriteOutcome AttendanceEventWriter::append_(
    const AttendanceMutationRequestId& requestId,
    AdminMutationReason reason, uint64_t sessionFingerprint,
    const Event& operation) {
  if (!ready_()) return result_(faulted_ ? AWS_UNCERTAIN : AWS_BLOCKED,
                                A_LOCKED, R_STATE);
  if (reason == AMR_NONE ||
      (reason != AMR_NONE &&
       (reason < AMR_DATA_ENTRY_ERROR || reason > AMR_OTHER_DOCUMENTED)) ||
      sessionFingerprint == 0 || sessionFingerprint > 0xFFFFFFFFFFFFULL)
    return result_(AWS_REJECTED, A_INVALID, R_BAD_FIELD);
  const size_t beforeCount = log_.count();
  const uint32_t beforeSeq = log_.lastSeq();
  const uint64_t generation = log_.openGeneration();
  if (beforeCount > static_cast<size_t>(-1) - 3u ||
      beforeSeq > UINT32_MAX - 4u)
    return result_(AWS_BLOCKED, A_LOCKED, R_FULL);

  Event group[3] = {
      Event{EV_ADMIN_MUTATION, FL_MORE, requestId.words[0],
            (static_cast<uint32_t>(requestId.words[2]) << 16) |
                static_cast<uint32_t>(requestId.words[3]),
            0, requestId.words[1]},
      Event{EV_ADMIN_AUDIT, FL_MORE, static_cast<uint16_t>(reason),
            static_cast<uint32_t>(sessionFingerprint & 0xFFFFFFFFu),
            0, static_cast<uint16_t>(sessionFingerprint >> 32)},
      operation};
  if (!log_.append(group, 3)) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }

  Event storedMarker = {};
  Event storedAudit = {};
  Event storedOperation = {};
  const bool markerRead = log_.at(beforeCount, storedMarker);
  const bool auditRead = log_.at(beforeCount + 1, storedAudit);
  const bool operationRead = log_.at(beforeCount + 2, storedOperation);
  const uint64_t storedFingerprint =
      static_cast<uint64_t>(storedAudit.epoch) |
      (static_cast<uint64_t>(storedAudit.b) << 32);
  const bool exact = markerRead && auditRead && operationRead &&
      storedMarker.seq == beforeSeq + 1u &&
      storedMarker.type == EV_ADMIN_MUTATION &&
      storedMarker.flags == FL_MORE &&
      sameRequestId(storedMarker, requestId) &&
      storedAudit.seq == beforeSeq + 2u &&
      storedAudit.type == EV_ADMIN_AUDIT && storedAudit.flags == FL_MORE &&
      storedAudit.a == static_cast<uint16_t>(reason) &&
      storedFingerprint == sessionFingerprint &&
      storedOperation.seq == beforeSeq + 3u &&
      sameOperation(storedOperation, operation) &&
      storedOperation.epoch == operation.epoch &&
      !flagMore(storedOperation.flags);
  const bool postcondition = exact && !log_.faulted() &&
      log_.openGeneration() == generation && generation == generation_ &&
      log_.count() == beforeCount + 3u &&
      log_.lastSeq() == beforeSeq + 3u;
  if (!postcondition) {
    faulted_ = true;
    log_.requireReopen();
    return result_(AWS_UNCERTAIN, A_LOCKED, R_STATE);
  }
  return result_(AWS_COMMITTED, A_OK, R_NONE, storedOperation.seq);
}

}  // namespace att
