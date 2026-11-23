#include "recovery_journal.h"
#include "backup_format.h"
#include "event_group_audit.h"
#include "log_index.h"
#include "roster_snapshot.h"
#include "sha256.h"
#include <new>
#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#endif
#include <string.h>

namespace att {
namespace {

void serviceRecoveryWatchdog() {
#if defined(ESP_PLATFORM)
  (void)esp_task_wdt_reset();
#endif
}

bool validMonth(uint32_t key) {
  const uint32_t year = key / 100u;
  const uint32_t month = key % 100u;
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12;
}

bool nonzeroId(const uint8_t* id) {
  if (!id) return false;
  uint8_t aggregate = 0;
  for (size_t i = 0; i < RECOVERY_OPERATION_ID_BYTES; ++i)
    aggregate |= id[i];
  return aggregate != 0;
}

void addLe32(Crc32& crc, uint32_t value) {
  const uint8_t bytes[4] = {
      (uint8_t)value, (uint8_t)(value >> 8),
      (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  crc.add(bytes, sizeof bytes);
}

void addLe64(Crc32& crc, uint64_t value) {
  uint8_t bytes[8];
  for (size_t i = 0; i < sizeof bytes; ++i)
    bytes[i] = (uint8_t)(value >> (8u * i));
  crc.add(bytes, sizeof bytes);
}

void previewFromPlan(RecoveryPreview& preview, const RecoveryPlan& plan) {
  preview.status = RECOVERY_PREVIEW_READY;
  memcpy(preview.operationId, plan.operationId,
         sizeof preview.operationId);
  preview.sourceCount = plan.sourceCount;
  preview.sourceTotalBytes = plan.sourceTotalBytes;
  preview.retainedBytes = plan.retainedBytes;
  memcpy(preview.sourceManifestSha256, plan.sourceManifestSha256,
         sizeof preview.sourceManifestSha256);
  preview.planCrc32 = plan.planCrc32;
}

bool previewMatchesPlan(const RecoveryPreview& preview,
                        const RecoveryPlan& plan) {
  return preview.status == RECOVERY_PREVIEW_READY &&
      memcmp(preview.operationId, plan.operationId,
             sizeof preview.operationId) == 0 &&
      preview.sourceCount == plan.sourceCount &&
      preview.sourceTotalBytes == plan.sourceTotalBytes &&
      preview.retainedBytes == plan.retainedBytes &&
      memcmp(preview.sourceManifestSha256, plan.sourceManifestSha256,
             sizeof preview.sourceManifestSha256) == 0 &&
      preview.planCrc32 == plan.planCrc32;
}

uint32_t calculateInventoryCrc(const MonthFileInventoryEntry* source,
                               size_t count) {
  Crc32 crc;
  uint8_t header[8] = {'M', 'S', 'I', 1, 0, 0, 0, 0};
  const uint32_t count32 = (uint32_t)count;
  for (size_t i = 0; i < 4; ++i)
    header[4 + i] = (uint8_t)(count32 >> (8u * i));
  crc.add(header, sizeof header);
  for (size_t i = 0; i < count; ++i) {
    addLe32(crc, source[i].monthKey);
    addLe64(crc, source[i].byteSize);
    addLe32(crc, source[i].contentCrc32);
    crc.add(source[i].contentSha256, sizeof source[i].contentSha256);
  }
  return crc.value();
}

void calculateInventorySha(const MonthFileInventoryEntry* source,
                           size_t count, uint8_t digest[32]) {
  Sha256 sha;
  uint8_t header[8] = {'M', 'S', 'I', 1, 0, 0, 0, 0};
  const uint32_t count32 = (uint32_t)count;
  for (size_t i = 0; i < 4; ++i)
    header[4 + i] = (uint8_t)(count32 >> (8u * i));
  sha.update(header, sizeof header);
  for (size_t i = 0; i < count; ++i) {
    uint8_t encoded[16];
    for (size_t b = 0; b < 4; ++b)
      encoded[b] = (uint8_t)(source[i].monthKey >> (8u * b));
    for (size_t b = 0; b < 8; ++b)
      encoded[4 + b] = (uint8_t)(source[i].byteSize >> (8u * b));
    for (size_t b = 0; b < 4; ++b)
      encoded[12 + b] = (uint8_t)(source[i].contentCrc32 >> (8u * b));
    sha.update(encoded, sizeof encoded);
    sha.update(source[i].contentSha256, sizeof source[i].contentSha256);
  }
  sha.finish(digest);
}

 }  // namespace

RecoveryPreviewGate::RecoveryPreviewGate() : issued_{}, kind_(KIND_NONE) {}

void RecoveryPreviewGate::issueInitial(const RecoveryPreview& preview) {
  clear();
  if (preview.status != RECOVERY_PREVIEW_READY) return;
  issued_ = preview;
  kind_ = KIND_INITIAL;
}

void RecoveryPreviewGate::issuePending(const RecoveryPreview& preview) {
  clear();
  if (preview.status != RECOVERY_PREVIEW_READY) return;
  issued_ = preview;
  kind_ = KIND_PENDING;
}

bool RecoveryPreviewGate::consumeInitial(const RecoveryPreview& preview) {
  return consume_(preview, KIND_INITIAL);
}

bool RecoveryPreviewGate::consumePending(const RecoveryPreview& preview) {
  return consume_(preview, KIND_PENDING);
}

bool RecoveryPreviewGate::consume_(const RecoveryPreview& preview,
                                   Kind expected) {
  const bool accepted = kind_ == expected && matches_(issued_, preview);
  clear();
  return accepted;
}

bool RecoveryPreviewGate::matches_(const RecoveryPreview& left,
                                   const RecoveryPreview& right) const {
  return left.status == RECOVERY_PREVIEW_READY &&
      right.status == RECOVERY_PREVIEW_READY &&
      memcmp(left.operationId, right.operationId,
             sizeof left.operationId) == 0 &&
      left.sourceCount == right.sourceCount &&
      left.sourceTotalBytes == right.sourceTotalBytes &&
      left.retainedBytes == right.retainedBytes &&
      memcmp(left.sourceManifestSha256, right.sourceManifestSha256,
             sizeof left.sourceManifestSha256) == 0 &&
      left.planCrc32 == right.planCrc32;
}

void RecoveryPreviewGate::clear() {
  memset(&issued_, 0, sizeof issued_);
  kind_ = KIND_NONE;
}

bool generateRecoveryOperationId(
    ISecureRandomSource& random,
    uint8_t out[RECOVERY_OPERATION_ID_BYTES]) {
  if (!out) return false;
  memset(out, 0, RECOVERY_OPERATION_ID_BYTES);
  if (!random.fill(out, RECOVERY_OPERATION_ID_BYTES)) {
    memset(out, 0, RECOVERY_OPERATION_ID_BYTES);
    return false;
  }
  uint8_t aggregate = 0;
  for (size_t i = 0; i < RECOVERY_OPERATION_ID_BYTES; ++i)
    aggregate |= out[i];
  if (aggregate == 0) {
    memset(out, 0, RECOVERY_OPERATION_ID_BYTES);
    return false;
  }
  return true;
}

uint32_t RecoveryJournal::calculatePlanCrc32(const RecoveryPlan& plan) {
  Crc32 crc;
  static const uint8_t magic[4] = {'A', 'T', 'R', 'J'};
  crc.add(magic, sizeof magic);
  addLe32(crc, plan.version);
  crc.add(plan.operationId, sizeof plan.operationId);
  addLe32(crc, (uint32_t)plan.sourceCount);
  addLe64(crc, plan.sourceTotalBytes);
  addLe64(crc, plan.retainedBytes);
  addLe32(crc, plan.sourceManifestCrc32);
  crc.add(plan.sourceManifestSha256, sizeof plan.sourceManifestSha256);
  for (size_t i = 0; i < plan.sourceCount && i < RECOVERY_MAX_FILES; ++i) {
    addLe32(crc, plan.source[i].monthKey);
    addLe64(crc, plan.source[i].byteSize);
    addLe32(crc, plan.source[i].contentCrc32);
    crc.add(plan.source[i].contentSha256,
            sizeof plan.source[i].contentSha256);
  }
  return crc.value();
}

bool RecoveryJournal::validatePlan(const RecoveryPlan& plan) {
  if (plan.version != RECOVERY_JOURNAL_VERSION ||
      !nonzeroId(plan.operationId) || plan.sourceCount == 0 ||
      plan.sourceCount > RECOVERY_MAX_FILES ||
      plan.retainedBytes >= plan.sourceTotalBytes ||
      (plan.retainedBytes % REC_SIZE) != 0u)
    return false;

  uint64_t total = 0;
  uint32_t previous = 0;
  for (size_t i = 0; i < plan.sourceCount; ++i) {
    const MonthFileInventoryEntry& entry = plan.source[i];
    if (!validMonth(entry.monthKey) ||
        (i != 0 && entry.monthKey <= previous) ||
        (i + 1 < plan.sourceCount &&
         (entry.byteSize % REC_SIZE) != 0u) ||
        entry.byteSize > UINT64_MAX - total)
      return false;
    total += entry.byteSize;
    previous = entry.monthKey;
  }

  uint8_t inventorySha[32];
  calculateInventorySha(plan.source, plan.sourceCount, inventorySha);
  return total == plan.sourceTotalBytes &&
      calculateInventoryCrc(plan.source, plan.sourceCount) ==
          plan.sourceManifestCrc32 &&
      memcmp(inventorySha, plan.sourceManifestSha256,
             sizeof inventorySha) == 0 &&
      calculatePlanCrc32(plan) == plan.planCrc32;
}

RecoveryResult RecoveryJournal::result_(RecoveryStatus status,
                                         const RecoveryPlan* plan) const {
  RecoveryResult result = {};
  result.status = status;
  if (plan) {
    memcpy(result.operationId, plan->operationId,
           sizeof result.operationId);
    result.retainedBytes = plan->retainedBytes;
  }
  return result;
}

RecoveryPreview RecoveryJournal::preview(
    const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
    MonthSet& source, int16_t utcOffsetMin) {
  RecoveryPreview preview = {};
  RecoveryPlan plan = {};
  preview.status = derivePlan_(operationId, source, utcOffsetMin, plan);
  if (preview.status != RECOVERY_PREVIEW_READY) return preview;

  previewFromPlan(preview, plan);
  return preview;
}

RecoveryResult RecoveryJournal::prepare(
    const RecoveryPreview& preview, MonthSet& source,
    int16_t utcOffsetMin, IRecoveryAuthorization& authorization) {
  if (preview.status != RECOVERY_PREVIEW_READY)
    return result_(RECOVERY_INVALID_REQUEST);
  return prepare_(preview, source, utcOffsetMin, authorization);
}

RecoveryStatus RecoveryJournal::derivePlan_(
    const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
    MonthSet& source, int16_t utcOffsetMin, RecoveryPlan& plan) {
  if (!nonzeroId(operationId) ||
      utcOffsetMin < -720 || utcOffsetMin > 840)
    return RECOVERY_INVALID_REQUEST;

  bool pendingFound = false;
  bool pendingAmbiguous = false;
  if (!backend_.findPending(plan, pendingFound, pendingAmbiguous))
    return RECOVERY_BACKEND_FAILURE;
  if (pendingAmbiguous) return RECOVERY_AMBIGUOUS_PENDING;
  if (pendingFound) return RECOVERY_PENDING_EXISTS;
  memset(&plan, 0, sizeof plan);

  // Re-scan the damaged stream now. Do not trust a caller-supplied prefix offset.
  LogStore diagnostic(source);
  const OpenInfo damaged = diagnostic.open();
  const bool diagnosedDamage = !damaged.ok &&
      (damaged.corruptAt >= 0 || damaged.tornBytes != 0 ||
       damaged.droppedGroup != 0);
  if (!diagnosedDamage ||
      damaged.completeGroupPrefixRecords > damaged.structurallyValidRecords ||
      damaged.droppedGroup != damaged.structurallyValidRecords -
                                  damaged.completeGroupPrefixRecords)
    return RECOVERY_INVALID_REQUEST;

  const uint64_t safePrefixBytes =
      (uint64_t)damaged.completeGroupPrefixRecords * REC_SIZE;
  plan.version = RECOVERY_JOURNAL_VERSION;
  memcpy(plan.operationId, operationId, sizeof plan.operationId);
  plan.retainedBytes = safePrefixBytes;

  MonthSetInventory before = {};
  if (!source.inspectInventory(plan.source, RECOVERY_MAX_FILES, before) ||
      before.fileCount == 0 || before.fileCount > RECOVERY_MAX_FILES ||
      safePrefixBytes > before.totalBytes ||
      safePrefixBytes >= before.totalBytes)
    return RECOVERY_INVALID_REQUEST;

  plan.sourceCount = before.fileCount;
  plan.sourceTotalBytes = before.totalBytes;
  plan.sourceManifestCrc32 = before.manifestCrc32;
  memcpy(plan.sourceManifestSha256, before.manifestSha256,
         sizeof plan.sourceManifestSha256);

  // The prefix is accepted only if every complete group passes the current
  // event schema, roster state machine, roster transition audit, and LogIndex
  // capacity checks. A CRC-valid unknown event is not treated as recoverable.
  EventGroupAudit* groups = new (std::nothrow) EventGroupAudit();
  RosterStore* roster = new (std::nothrow) RosterStore(utcOffsetMin);
  RosterTransitionAudit* transitions =
      new (std::nothrow) RosterTransitionAudit();
  LogIndex* index = new (std::nothrow) LogIndex();
  bool semanticOk = groups && roster && transitions && index;
  uint8_t record[REC_SIZE];
  if (semanticOk) {
    groups->onReset();
    roster->onReset();
    transitions->reset();
    index->onReset();
    for (uint32_t i = 0; i < damaged.completeGroupPrefixRecords; ++i) {
      if ((i & 0x3fu) == 0) serviceRecoveryWatchdog();
      Event event;
      if (!source.read((size_t)i * REC_SIZE, record, sizeof record) ||
          !decode(record, event) || event.seq != i + 1u) {
        semanticOk = false;
        break;
      }
      groups->onRecord(event);
      roster->onRecord(event);
      index->onRecord(event);
      if (event.type == EV_ROSTER &&
          !transitions->consume(event, utcOffsetMin)) {
        semanticOk = false;
        break;
      }
      // A valid multi-record group and roster SET are intentionally pending
      // until their final payload record. Reject malformed state immediately,
      // but defer healthy()/historyComplete() until the full prefix ends.
      if (groups->malformed() || groups->sequenceError() ||
          !transitions->valid() || index->gapOverflow() ||
          index->closeOverflow()) {
        semanticOk = false;
        break;
      }
    }
    semanticOk = semanticOk && groups->healthy() &&
        groups->recordsSeen() == damaged.completeGroupPrefixRecords &&
        groups->lastSeq() == damaged.completeGroupPrefixRecords &&
        roster->historyComplete() && transitions->valid() &&
        transitions->matches(*roster) &&
        !index->gapOverflow() && !index->closeOverflow();
  }
  delete index;
  delete transitions;
  delete roster;
  delete groups;
  if (!semanticOk) return RECOVERY_INVALID_REQUEST;

  // Re-fingerprint after the prefix audit. The SHA-256 manifest must be
  // identical; otherwise another writer changed the source during inspection.
  MonthSetInventory after = {};
  if (!source.inspectInventory(plan.source, RECOVERY_MAX_FILES, after) ||
      after.fileCount != before.fileCount ||
      after.totalBytes != before.totalBytes ||
      after.manifestCrc32 != before.manifestCrc32 ||
      memcmp(after.manifestSha256, before.manifestSha256,
             sizeof before.manifestSha256) != 0)
    return RECOVERY_SOURCE_CHANGED;

  plan.planCrc32 = calculatePlanCrc32(plan);
  if (!validatePlan(plan)) return RECOVERY_INVALID_REQUEST;
  return RECOVERY_PREVIEW_READY;
}

RecoveryResult RecoveryJournal::prepare_(
    const RecoveryPreview& expectedPreview, MonthSet& source,
    int16_t utcOffsetMin, IRecoveryAuthorization& authorization) {
  if (!backend_.writable()) return result_(RECOVERY_READ_ONLY);

  RecoveryPlan plan = {};
  const RecoveryStatus derived = derivePlan_(
      expectedPreview.operationId, source, utcOffsetMin, plan);
  if (derived != RECOVERY_PREVIEW_READY) return result_(derived);

  if (!previewMatchesPlan(expectedPreview, plan))
    return result_(RECOVERY_STALE_PREVIEW, &plan);

  if (!authorization.authorize(plan.operationId, plan.retainedBytes) ||
      !authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  if (!backend_.capacityAvailable(plan, plan.sourceTotalBytes))
    return result_(RECOVERY_ARCHIVE_FAILURE, &plan);
  if (!authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  if (!backend_.writeIntent(plan))
    return result_(RECOVERY_BACKEND_FAILURE, &plan);
  return resume_(plan, authorization);
}

RecoveryPreview RecoveryJournal::pendingPreview() {
  RecoveryPreview preview = {};
  RecoveryPlan plan = {};
  bool found = false;
  bool ambiguous = false;
  if (!backend_.findPending(plan, found, ambiguous)) {
    preview.status = RECOVERY_BACKEND_FAILURE;
    return preview;
  }
  if (ambiguous) {
    preview.status = RECOVERY_AMBIGUOUS_PENDING;
    return preview;
  }
  if (!found) {
    preview.status = RECOVERY_NO_PENDING;
    return preview;
  }
  if (!validatePlan(plan)) {
    preview.status = RECOVERY_BACKEND_FAILURE;
    return preview;
  }
  previewFromPlan(preview, plan);
  return preview;
}

RecoveryResult RecoveryJournal::resumePending(
    const RecoveryPreview& preview, IRecoveryAuthorization& authorization) {
  if (preview.status != RECOVERY_PREVIEW_READY)
    return result_(RECOVERY_INVALID_REQUEST);
  return resumePending_(preview, authorization);
}

RecoveryResult RecoveryJournal::resumePending_(
    const RecoveryPreview& expectedPreview,
    IRecoveryAuthorization& authorization) {
  RecoveryPlan plan = {};
  bool found = false;
  bool ambiguous = false;
  if (!backend_.findPending(plan, found, ambiguous))
    return result_(RECOVERY_BACKEND_FAILURE);
  if (ambiguous) return result_(RECOVERY_AMBIGUOUS_PENDING);
  if (!found) return result_(RECOVERY_NO_PENDING);
  if (!validatePlan(plan)) return result_(RECOVERY_BACKEND_FAILURE);
  if (!previewMatchesPlan(expectedPreview, plan))
    return result_(RECOVERY_STALE_PREVIEW, &plan);
  if (!backend_.writable()) return result_(RECOVERY_READ_ONLY, &plan);
  if (!authorization.authorize(plan.operationId, plan.retainedBytes) ||
      !authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);

  bool complete = false;
  if (!backend_.completeMarker(plan, complete))
    return result_(RECOVERY_BACKEND_FAILURE, &plan);
  if (complete) return result_(RECOVERY_COMPLETE, &plan);
  return resume_(plan, authorization);
}

RecoveryResult RecoveryJournal::resume_(
    const RecoveryPlan& plan, IRecoveryAuthorization& authorization) {
  if (!backend_.writable()) return result_(RECOVERY_READ_ONLY, &plan);
  if (!validatePlan(plan)) return result_(RECOVERY_INVALID_REQUEST, &plan);

  bool complete = false;
  if (!backend_.completeMarker(plan, complete))
    return result_(RECOVERY_BACKEND_FAILURE, &plan);
  if (complete) return result_(RECOVERY_COMPLETE, &plan);

  bool ready = false;
  if (!backend_.archiveReady(plan, ready))
    return result_(RECOVERY_ARCHIVE_FAILURE, &plan);

  RecoveryResult result = result_(RECOVERY_ARCHIVE_FAILURE, &plan);
  if (!authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  if (!ready) {
    uint64_t additionalBytes = 0;
    for (size_t i = 0; i < plan.sourceCount; ++i) {
      if (!backend_.verifyArchive(plan, i)) {
        if (plan.source[i].byteSize > UINT64_MAX - additionalBytes)
          return result_(RECOVERY_ARCHIVE_FAILURE, &plan);
        additionalBytes += plan.source[i].byteSize;
      }
    }
    if (!backend_.capacityAvailable(plan, additionalBytes))
      return result_(RECOVERY_ARCHIVE_FAILURE, &plan);
    for (size_t i = 0; i < plan.sourceCount; ++i) {
      if (!authorization.continueAuthorized())
        return result_(RECOVERY_AUTH_REQUIRED, &plan);
      if (!backend_.ensureArchiveWithLease(plan, i, authorization)) {
        if (!authorization.continueAuthorized())
          return result_(RECOVERY_AUTH_REQUIRED, &plan);
        return result;
      }
      if (!backend_.verifyArchive(plan, i)) {
        if (!authorization.continueAuthorized())
          return result_(RECOVERY_AUTH_REQUIRED, &plan);
        return result;
      }
      if (!authorization.continueAuthorized())
        return result_(RECOVERY_AUTH_REQUIRED, &plan);
      ++result.filesArchived;
    }
    if (!authorization.continueAuthorized())
      return result_(RECOVERY_AUTH_REQUIRED, &plan);
    if (!backend_.writeArchiveReady(plan)) {
      if (!authorization.continueAuthorized())
        return result_(RECOVERY_AUTH_REQUIRED, &plan);
      return result;
    }
    if (!authorization.continueAuthorized())
      return result_(RECOVERY_AUTH_REQUIRED, &plan);
  } else {
    for (size_t i = 0; i < plan.sourceCount; ++i) {
      if (!authorization.continueAuthorized())
        return result_(RECOVERY_AUTH_REQUIRED, &plan);
      if (!backend_.verifyArchive(plan, i)) return result;
      ++result.filesArchived;
    }
  }

  uint64_t remaining = plan.retainedBytes;
  for (size_t i = 0; i < plan.sourceCount; ++i) {
    if (!authorization.continueAuthorized())
      return result_(RECOVERY_AUTH_REQUIRED, &plan);
    const MonthFileInventoryEntry& entry = plan.source[i];
    const uint64_t keep = remaining < entry.byteSize
        ? remaining : entry.byteSize;
    remaining -= keep;

    bool exists = false;
    uint64_t activeSize = 0;
    if (!backend_.activeFileState(entry.monthKey, exists, activeSize))
      return result_(RECOVERY_BACKEND_FAILURE, &plan);

    if (keep == 0) {
      if (exists) {
        if (!backend_.removeActiveFile(plan, i)) {
          if (!authorization.continueAuthorized())
            return result_(RECOVERY_AUTH_REQUIRED, &plan);
          return result_(RECOVERY_ACTIVE_MISMATCH, &plan);
        }
        if (!authorization.continueAuthorized())
          return result_(RECOVERY_AUTH_REQUIRED, &plan);
        ++result.filesChanged;
      }
      continue;
    }

    if (!exists || activeSize != keep ||
        !backend_.activeMatchesArchivePrefix(plan, i, keep)) {
      if (!backend_.replaceActiveFromArchiveWithLease(
              plan, i, keep, authorization)) {
        if (!authorization.continueAuthorized())
          return result_(RECOVERY_AUTH_REQUIRED, &plan);
        return result_(RECOVERY_ACTIVE_MISMATCH, &plan);
      }
      if (!authorization.continueAuthorized())
        return result_(RECOVERY_AUTH_REQUIRED, &plan);
      ++result.filesChanged;
    }
  }

  if (!authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  if (remaining != 0 || !backend_.verifyActiveResult(plan))
    return result_(RECOVERY_ACTIVE_MISMATCH, &plan);
  if (!authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  if (!backend_.writeCompleteMarker(plan)) {
    if (!authorization.continueAuthorized())
      return result_(RECOVERY_AUTH_REQUIRED, &plan);
    return result_(RECOVERY_BACKEND_FAILURE, &plan);
  }
  if (!authorization.continueAuthorized())
    return result_(RECOVERY_AUTH_REQUIRED, &plan);
  return result_(RECOVERY_COMPLETE, &plan);
}

}  // namespace att
