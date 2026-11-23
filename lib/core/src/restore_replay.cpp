#include "restore_replay.h"

namespace att {
namespace {
void wipeConfigBytes(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}
}

RestoreReplayObserverFanout::RestoreReplayObserverFanout(
    RosterStore& roster, LogIndex& index, RestoreGroupAudit& groups)
    : roster_(roster), index_(index), groups_(groups), recordsSeen_(0) {}

void RestoreReplayObserverFanout::onReset() {
  roster_.onReset();
  index_.onReset();
  groups_.onReset();
  recordsCrc_ = Crc32();
  recordsSeen_ = 0;
}

void RestoreReplayObserverFanout::onRecord(const Event& event) {
  uint8_t encoded[REC_SIZE];
  encode(event, encoded);
  recordsCrc_.add(encoded, sizeof encoded);
  if (recordsSeen_ != UINT32_MAX) ++recordsSeen_;
  roster_.onRecord(event);
  groups_.onRecord(event);
  index_.onRecord(event);
}

RestoreReplayAudit::RestoreReplayAudit(IStorage& staged,
                                       const RestoreValidator& validator)
    : staged_(staged), validator_(validator), config_(validator.config()),
      replayLog_(staged_), roster_(config_.params.utcOffsetMin), index_(),
      groups_(), observers_(roster_, index_, groups_),
      validatedAtConstruction_(validator.ok()), ran_(false),
      result_{RESTORE_REPLAY_NOT_VALIDATED, 0, 0, 0, false, false, false} {}

RestoreReplayAudit::~RestoreReplayAudit() {
  wipeConfigBytes(&config_, sizeof config_);
}

RestoreReplayResult RestoreReplayAudit::finish_(RestoreReplayCode code,
                                                const OpenInfo* opened) {
  result_.code = code;
  result_.malformedRoster = roster_.malformed();
  result_.rosterOverflow = roster_.overflow();
  result_.gapOverflow = index_.gapOverflow();
  result_.closeOverflow = index_.closeOverflow();
  if (opened) {
    result_.records = opened->records;
    result_.lastSeq = opened->lastSeq;
  }
  return result_;
}

RestoreReplayResult RestoreReplayAudit::run() {
  if (ran_) return result_;
  ran_ = true;

  if (!validatedAtConstruction_ || !validator_.ok())
    return finish_(RESTORE_REPLAY_NOT_VALIDATED);
  if (validator_.stagingStorage() != &staged_)
    return finish_(RESTORE_REPLAY_STORAGE_MISMATCH);
  size_t stagedSize = 0;
  if (!staged_.sizeChecked(stagedSize))
    return finish_(RESTORE_REPLAY_STAGE_SIZE_FAILED);
  if ((uint64_t)validator_.records() > (uint64_t)SIZE_MAX / REC_SIZE ||
      stagedSize != (size_t)validator_.records() * REC_SIZE)
    return finish_(RESTORE_REPLAY_SIZE_MISMATCH);

  // Preserve the precise staging-read failure result and verify the bytes
  // accepted by RestoreValidator before structural open. The fan-out performs
  // a second fingerprint over the exact later observer replay to close TOCTOU.
  Crc32 stagedRecordsCrc;
  uint8_t record[REC_SIZE];
  for (uint32_t i = 0; i < validator_.records(); ++i) {
    if (!staged_.read((size_t)i * REC_SIZE, record, REC_SIZE))
      return finish_(RESTORE_REPLAY_STAGE_READ_FAILED);
    stagedRecordsCrc.add(record, REC_SIZE);
  }
  if (stagedRecordsCrc.value() != validator_.recordsCrc())
    return finish_(RESTORE_REPLAY_STAGE_CHANGED);

  // LogStore rejects observer attachment before open(). Open the structurally
  // validated prefix first, then replay it once through the persistent fan-out.
  // The fan-out fingerprints the exact decoded records delivered to every index,
  // closing the gap between a separate staging read and the actual replay pass.
  // This CRC detects accidental mutation; it is not a cryptographic MAC.
  const OpenInfo opened = replayLog_.open();
  if (!opened.ok || opened.corruptAt >= 0 || opened.tornBytes != 0 ||
      opened.droppedGroup != 0)
    return finish_(RESTORE_REPLAY_LOG_INVALID, &opened);

  // The validator's authenticated record count/sequence are authoritative. LogStore
  // must reopen exactly that stream without repairing, dropping, or inventing records.
  if (opened.records != validator_.records() ||
      opened.lastSeq != validator_.lastSeq())
    return finish_(RESTORE_REPLAY_RECORD_MISMATCH, &opened);

  // Attach replays the already-opened validated prefix into roster, group audit,
  // and index in the same ordered pass. Semantic failures remain inspectable here.
  if (!replayLog_.attach(&observers_))
    return finish_(RESTORE_REPLAY_ATTACH_FAILED, &opened);
  if (!observers_.matchesRecords(validator_.records(), validator_.recordsCrc()))
    return finish_(RESTORE_REPLAY_STAGE_CHANGED, &opened);

  if (groups_.malformed() || groups_.pending() || groups_.sequenceError())
    return finish_(RESTORE_REPLAY_GROUP_MALFORMED, &opened);
  if (roster_.malformed() != 0)
    return finish_(RESTORE_REPLAY_ROSTER_MALFORMED, &opened);
  if (roster_.overflow())
    return finish_(RESTORE_REPLAY_ROSTER_OVERFLOW, &opened);
  if (index_.gapOverflow())
    return finish_(RESTORE_REPLAY_GAP_OVERFLOW, &opened);
  if (index_.closeOverflow())
    return finish_(RESTORE_REPLAY_CLOSE_OVERFLOW, &opened);

  return finish_(RESTORE_REPLAY_OK, &opened);
}

}  // namespace att
