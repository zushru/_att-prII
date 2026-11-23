#include "restore_transaction.h"
#include <string.h>

namespace att {
namespace {

void secureClear(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

bool nonzero(const uint8_t* bytes, size_t length) {
  if (!bytes) return false;
  uint8_t aggregate = 0;
  for (size_t i = 0; i < length; ++i) aggregate |= bytes[i];
  return aggregate != 0;
}

void crcLe32(Crc32& crc, uint32_t value) {
  const uint8_t bytes[4] = {
      static_cast<uint8_t>(value),
      static_cast<uint8_t>(value >> 8),
      static_cast<uint8_t>(value >> 16),
      static_cast<uint8_t>(value >> 24)};
  crc.add(bytes, sizeof bytes);
}

void crcLe64(Crc32& crc, uint64_t value) {
  const uint8_t bytes[8] = {
      static_cast<uint8_t>(value),
      static_cast<uint8_t>(value >> 8),
      static_cast<uint8_t>(value >> 16),
      static_cast<uint8_t>(value >> 24),
      static_cast<uint8_t>(value >> 32),
      static_cast<uint8_t>(value >> 40),
      static_cast<uint8_t>(value >> 48),
      static_cast<uint8_t>(value >> 56)};
  crc.add(bytes, sizeof bytes);
}

void putLe16(uint8_t*& out, uint16_t value) {
  *out++ = static_cast<uint8_t>(value);
  *out++ = static_cast<uint8_t>(value >> 8);
}

void putLe32(uint8_t*& out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    *out++ = static_cast<uint8_t>(value >> (8u * i));
}

void putLe64(uint8_t*& out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    *out++ = static_cast<uint8_t>(value >> (8u * i));
}

uint16_t getLe16(const uint8_t*& in) {
  const uint16_t value = static_cast<uint16_t>(in[0]) |
      static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
  in += 2;
  return value;
}

uint32_t getLe32(const uint8_t*& in) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= static_cast<uint32_t>(*in++) << (8u * i);
  return value;
}

uint64_t getLe64(const uint8_t*& in) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(*in++) << (8u * i);
  return value;
}

bool sameJournal(const RestoreTxJournal& a, const RestoreTxJournal& b) {
  return a.version == b.version && a.phase == b.phase &&
      a.targetConfigSlot == b.targetConfigSlot && a.reserved == b.reserved &&
      memcmp(a.operationId, b.operationId, sizeof a.operationId) == 0 &&
      a.records == b.records && a.lastSeq == b.lastSeq &&
      a.recordsCrc32 == b.recordsCrc32 && a.configSeq == b.configSeq &&
      memcmp(a.configSlot, b.configSlot, sizeof a.configSlot) == 0 &&
      a.priorLogFileCount == b.priorLogFileCount &&
      a.priorLogTotalBytes == b.priorLogTotalBytes &&
      memcmp(a.priorLogManifestSha256, b.priorLogManifestSha256,
             sizeof a.priorLogManifestSha256) == 0 &&
      a.journalCrc32 == b.journalCrc32;
}

}  // namespace

uint32_t calculateRestoreTxJournalCrc32(const RestoreTxJournal& journal) {
  Crc32 crc;
  crcLe32(crc, journal.version);
  crc.add(&journal.phase, sizeof journal.phase);
  crc.add(&journal.targetConfigSlot, sizeof journal.targetConfigSlot);
  const uint8_t reservedBytes[2] = {
      static_cast<uint8_t>(journal.reserved),
      static_cast<uint8_t>(journal.reserved >> 8)};
  crc.add(reservedBytes, sizeof reservedBytes);
  crc.add(journal.operationId, sizeof journal.operationId);
  crcLe32(crc, journal.records);
  crcLe32(crc, journal.lastSeq);
  crcLe32(crc, journal.recordsCrc32);
  crcLe32(crc, journal.configSeq);
  crc.add(journal.configSlot, sizeof journal.configSlot);
  crcLe32(crc, journal.priorLogFileCount);
  crcLe64(crc, journal.priorLogTotalBytes);
  crc.add(journal.priorLogManifestSha256,
          sizeof journal.priorLogManifestSha256);
  return crc.value();
}

bool validateRestoreTxJournal(const RestoreTxJournal& journal) {
  if (journal.version != RESTORE_TX_VERSION ||
      journal.phase < RESTORE_TX_PHASE_PREPARED ||
      journal.phase > RESTORE_TX_PHASE_ACKNOWLEDGED ||
      journal.targetConfigSlot > 1 || journal.reserved != 0 ||
      !nonzero(journal.operationId, sizeof journal.operationId) ||
      journal.records > BK_RECORD_MAX ||
      ((journal.records == 0) != (journal.lastSeq == 0)) ||
      (journal.records != 0 && journal.records != journal.lastSeq) ||
      journal.configSeq == 0 ||
      (journal.priorLogFileCount == 0 && journal.priorLogTotalBytes != 0) ||
      !nonzero(journal.priorLogManifestSha256,
               sizeof journal.priorLogManifestSha256) ||
      journal.journalCrc32 != calculateRestoreTxJournalCrc32(journal))
    return false;

  Config decoded;
  uint32_t seq = 0;
  const bool valid = decodeConfig(journal.configSlot, decoded, seq) == CD_OK &&
      seq == journal.configSeq;
  secureClear(&decoded, sizeof decoded);
  return valid;
}

bool encodeRestoreTxJournal(const RestoreTxJournal& journal,
                            uint8_t out[RESTORE_TX_JOURNAL_BYTES]) {
  if (!out || !validateRestoreTxJournal(journal)) return false;
  uint8_t* cursor = out;
  putLe32(cursor, journal.version);
  *cursor++ = journal.phase;
  *cursor++ = journal.targetConfigSlot;
  putLe16(cursor, journal.reserved);
  memcpy(cursor, journal.operationId, sizeof journal.operationId);
  cursor += sizeof journal.operationId;
  putLe32(cursor, journal.records);
  putLe32(cursor, journal.lastSeq);
  putLe32(cursor, journal.recordsCrc32);
  putLe32(cursor, journal.configSeq);
  memcpy(cursor, journal.configSlot, sizeof journal.configSlot);
  cursor += sizeof journal.configSlot;
  putLe32(cursor, journal.priorLogFileCount);
  putLe64(cursor, journal.priorLogTotalBytes);
  memcpy(cursor, journal.priorLogManifestSha256,
         sizeof journal.priorLogManifestSha256);
  cursor += sizeof journal.priorLogManifestSha256;
  putLe32(cursor, journal.journalCrc32);
  return static_cast<size_t>(cursor - out) == RESTORE_TX_JOURNAL_BYTES;
}

bool decodeRestoreTxJournal(const uint8_t* bytes, size_t length,
                            RestoreTxJournal& out) {
  secureClear(&out, sizeof out);
  if (!bytes || length != RESTORE_TX_JOURNAL_BYTES) return false;
  const uint8_t* cursor = bytes;
  out.version = getLe32(cursor);
  out.phase = *cursor++;
  out.targetConfigSlot = *cursor++;
  out.reserved = getLe16(cursor);
  memcpy(out.operationId, cursor, sizeof out.operationId);
  cursor += sizeof out.operationId;
  out.records = getLe32(cursor);
  out.lastSeq = getLe32(cursor);
  out.recordsCrc32 = getLe32(cursor);
  out.configSeq = getLe32(cursor);
  memcpy(out.configSlot, cursor, sizeof out.configSlot);
  cursor += sizeof out.configSlot;
  out.priorLogFileCount = getLe32(cursor);
  out.priorLogTotalBytes = getLe64(cursor);
  memcpy(out.priorLogManifestSha256, cursor,
         sizeof out.priorLogManifestSha256);
  cursor += sizeof out.priorLogManifestSha256;
  out.journalCrc32 = getLe32(cursor);
  if (static_cast<size_t>(cursor - bytes) != length ||
      !validateRestoreTxJournal(out)) {
    secureClear(&out, sizeof out);
    return false;
  }
  return true;
}

bool generateRestoreTxOperationId(
    ISecureRandomSource& random,
    uint8_t out[RESTORE_TX_OPERATION_ID_BYTES]) {
  if (!out) return false;
  memset(out, 0, RESTORE_TX_OPERATION_ID_BYTES);
  if (!random.fill(out, RESTORE_TX_OPERATION_ID_BYTES) ||
      !nonzero(out, RESTORE_TX_OPERATION_ID_BYTES)) {
    secureClear(out, RESTORE_TX_OPERATION_ID_BYTES);
    return false;
  }
  return true;
}

RestoreTransactionCoordinator::~RestoreTransactionCoordinator() {
  wipe_();
}

void RestoreTransactionCoordinator::wipe_() {
  secureClear(&journal_, sizeof journal_);
  staged_ = nullptr;
  hasJournal_ = false;
}

RestoreTxResult RestoreTransactionCoordinator::result_(RestoreTxCode code) const {
  return RestoreTxResult{
      code,
      hasJournal_ ? static_cast<RestoreTxPhase>(journal_.phase)
                  : RESTORE_TX_PHASE_NONE,
      hasJournal_ ? journal_.records : 0,
      hasJournal_ ? journal_.lastSeq : 0,
      hasJournal_ ? journal_.configSeq : 0};
}

bool RestoreTransactionCoordinator::loadJournal_() {
  if (journalLoaded_) return true;
  RestoreTxJournal loaded = {};
  bool present = false;
  bool ambiguous = false;
  if (!backend_.readJournal(loaded, present, ambiguous)) {
    loadError_ = RESTORE_TX_JOURNAL_IO;
    secureClear(&loaded, sizeof loaded);
    return false;
  }
  if (ambiguous) {
    loadError_ = RESTORE_TX_JOURNAL_AMBIGUOUS;
    secureClear(&loaded, sizeof loaded);
    faulted_ = true;
    return false;
  }
  if (present && !validateRestoreTxJournal(loaded)) {
    loadError_ = RESTORE_TX_JOURNAL_CORRUPT;
    secureClear(&loaded, sizeof loaded);
    faulted_ = true;
    return false;
  }
  secureClear(&journal_, sizeof journal_);
  if (present) memcpy(&journal_, &loaded, sizeof journal_);
  secureClear(&loaded, sizeof loaded);
  hasJournal_ = present;
  journalLoaded_ = true;
  loadError_ = RESTORE_TX_NO_PENDING;
  return true;
}

bool RestoreTransactionCoordinator::persistPhase_(RestoreTxPhase phase) {
  if (!hasJournal_ || phase < journal_.phase ||
      phase > RESTORE_TX_PHASE_ACKNOWLEDGED) return false;

  RestoreTxJournal next = journal_;
  next.phase = static_cast<uint8_t>(phase);
  next.journalCrc32 = calculateRestoreTxJournalCrc32(next);
  if (!backend_.writeJournal(next)) {
    secureClear(&next, sizeof next);
    faulted_ = true;
    return false;
  }

  RestoreTxJournal readback = {};
  bool present = false;
  bool ambiguous = false;
  const bool readOk = backend_.readJournal(readback, present, ambiguous);
  const bool matches = readOk && present && !ambiguous &&
      validateRestoreTxJournal(readback) && sameJournal(readback, next);
  if (!matches) {
    secureClear(&readback, sizeof readback);
    secureClear(&next, sizeof next);
    faulted_ = true;
    return false;
  }
  secureClear(&journal_, sizeof journal_);
  memcpy(&journal_, &next, sizeof journal_);
  secureClear(&readback, sizeof readback);
  secureClear(&next, sizeof next);
  return true;
}

bool RestoreTransactionCoordinator::authorizationAlive_(
    IRestoreTransactionAuthorization* authorization) const {
  return authorization == nullptr || authorization->continueAuthorized();
}

RestoreTxResult RestoreTransactionCoordinator::begin(
    BackupReader& outerReader, RestoreValidator& validator,
    RestoreReplayAudit& replay,
    const uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES],
    IRestoreTransactionAuthorization& authorization) {
  if (faulted_) return result_(RESTORE_TX_FAULTED);
  if (!outerReader.done() || !outerReader.writesTo(validator) ||
      !validator.ok() || !replay.usesValidator(validator))
    return result_(RESTORE_TX_INVALID_EVIDENCE);
  if (!backend_.writable() || !operationId ||
      !nonzero(operationId, RESTORE_TX_OPERATION_ID_BYTES))
    return result_(RESTORE_TX_NOT_READY);
  const RestoreReplayResult replayResult = replay.run();
  if (replayResult.code != RESTORE_REPLAY_OK ||
      replayResult.records != validator.records() ||
      replayResult.lastSeq != validator.lastSeq())
    return result_(RESTORE_TX_INVALID_EVIDENCE);

  if (!loadJournal_()) return result_(loadError_);
  if (hasJournal_) {
    if (!backend_.closeWritersAndDrain()) faulted_ = true;
    return result_(RESTORE_TX_NOT_READY);
  }
  if (!authorization.authorizeStart(operationId, validator.lastSeq()))
    return result_(RESTORE_TX_AUTH_DENIED);
  if (!backend_.closeWritersAndDrain()) {
    faulted_ = true;
    return result_(RESTORE_TX_NOT_READY);
  }
  staged_ = validator.stagingStorage();
  if (!staged_) return result_(RESTORE_TX_STAGE_MISMATCH);

  RestoreTxJournal next = {};
  next.version = RESTORE_TX_VERSION;
  next.phase = RESTORE_TX_PHASE_PREPARED;
  next.reserved = 0;
  memcpy(next.operationId, operationId, sizeof next.operationId);
  next.records = validator.records();
  next.lastSeq = validator.lastSeq();
  next.recordsCrc32 = validator.recordsCrc();
  next.configSeq = validator.cfgSeq();
  // Bind the retained pre-restore backup to the exact live file inventory
  // before writing any transaction intent. Writers are already quiesced.
  if (!backend_.capturePriorLogManifest(next) ||
      (next.priorLogFileCount == 0 && next.priorLogTotalBytes != 0) ||
      !nonzero(next.priorLogManifestSha256,
               sizeof next.priorLogManifestSha256)) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    return result_(RESTORE_TX_BACKUP_VERIFY_FAILED);
  }

  // Snapshot both current config slots and choose a generation newer than both
  // current and restored data. The previous active slot is left untouched.
  uint8_t rawA[CFG_SLOT_SIZE];
  uint8_t rawB[CFG_SLOT_SIZE];
  Config current;
  Config decodedA;
  Config decodedB;
  secureClear(&current, sizeof current);
  secureClear(&decodedA, sizeof decodedA);
  secureClear(&decodedB, sizeof decodedB);
  bool configOk = configSlots_.readSlot(0, rawA) &&
      configSlots_.readSlot(1, rawB);
  if (configOk) {
    uint32_t seqA = 0;
    uint32_t seqB = 0;
    const CfgDec da = decodeConfig(rawA, decodedA, seqA);
    const CfgDec db = decodeConfig(rawB, decodedB, seqB);
    configOk = (da == CD_OK || da == CD_BLANK) &&
        (db == CD_OK || db == CD_BLANK);
    if (configOk) {
      const CfgPick pick = pickSlot(rawA, rawB, current);
      configOk = pick.boot != CB_FAULT;
      if (configOk) {
        uint32_t maximum = validator.cfgSeq();
        if (da == CD_OK && seqA > maximum) maximum = seqA;
        if (db == CD_OK && seqB > maximum) maximum = seqB;
        configOk = maximum != UINT32_MAX;
        if (configOk) {
          next.configSeq = maximum + 1;
          next.targetConfigSlot = pick.boot == CB_OK
              ? static_cast<uint8_t>(1 - pick.active) : 0;
          configOk = encodeConfig(validator.config(), next.configSeq,
                                  next.configSlot) != 0;
        }
      }
    }
  }
  secureClear(rawA, sizeof rawA);
  secureClear(rawB, sizeof rawB);
  secureClear(&current, sizeof current);
  secureClear(&decodedA, sizeof decodedA);
  secureClear(&decodedB, sizeof decodedB);
  if (!configOk) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    return result_(RESTORE_TX_CONFIG_FAULT);
  }

  next.journalCrc32 = calculateRestoreTxJournalCrc32(next);
  if (!validateRestoreTxJournal(next) ||
      !backend_.verifyStagedLog(*staged_, next)) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    return result_(RESTORE_TX_STAGE_MISMATCH);
  }
  if (!authorization.continueAuthorized()) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    return result_(RESTORE_TX_PRESENCE_LOST);
  }
  if (!backend_.writeJournal(next)) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    faulted_ = true;
    return result_(RESTORE_TX_JOURNAL_IO);
  }
  RestoreTxJournal readback = {};
  bool journalPresent = false;
  bool journalAmbiguous = false;
  const bool readOk = backend_.readJournal(readback, journalPresent,
                                           journalAmbiguous);
  const bool journalMatches = readOk && journalPresent && !journalAmbiguous &&
      validateRestoreTxJournal(readback) && sameJournal(readback, next);
  secureClear(&readback, sizeof readback);
  if (!journalMatches) {
    secureClear(&next, sizeof next);
    staged_ = nullptr;
    faulted_ = true;
    return result_(RESTORE_TX_JOURNAL_IO);
  }
  secureClear(&journal_, sizeof journal_);
  memcpy(&journal_, &next, sizeof journal_);
  secureClear(&next, sizeof next);
  journalLoaded_ = true;
  hasJournal_ = true;
  return resume_(&authorization);
}

RestoreTxResult RestoreTransactionCoordinator::recoverPending(
    IRestoreTransactionAuthorization& authorization) {
  if (faulted_) return result_(RESTORE_TX_FAULTED);
  if (!backend_.writable()) return result_(RESTORE_TX_NOT_READY);
  if (!loadJournal_()) return result_(loadError_);
  if (!hasJournal_) return result_(RESTORE_TX_NO_PENDING);
  if (!validateRestoreTxJournal(journal_)) {
    faulted_ = true;
    return result_(RESTORE_TX_JOURNAL_CORRUPT);
  }
  if (!backend_.closeWritersAndDrain()) {
    faulted_ = true;
    return result_(RESTORE_TX_NOT_READY);
  }
  if (journal_.phase == RESTORE_TX_PHASE_COMMITTED)
    return result_(RESTORE_TX_AWAITING_ACK);
  if (journal_.phase == RESTORE_TX_PHASE_ACKNOWLEDGED)
    return finishAcknowledged_();
  if (!authorization.authorizeResume(journal_.operationId, journal_.lastSeq))
    return result_(RESTORE_TX_AUTH_DENIED);
  if (journal_.phase <= RESTORE_TX_PHASE_BACKUP_READY) {
    staged_ = backend_.openStagedLog(journal_);
    if (!staged_) return result_(RESTORE_TX_STAGE_MISMATCH);
  }
  return resume_(&authorization);
}

RestoreTxResult RestoreTransactionCoordinator::resume_(
    IRestoreTransactionAuthorization* authorization) {
  if (faulted_ || !hasJournal_) return result_(RESTORE_TX_FAULTED);
  if (!backend_.writable() || !backend_.closeWritersAndDrain()) {
    faulted_ = true;
    return result_(RESTORE_TX_NOT_READY);
  }

  if (journal_.phase == RESTORE_TX_PHASE_PREPARED) {
    if (!authorizationAlive_(authorization))
      return result_(RESTORE_TX_PRESENCE_LOST);
    if (!staged_ || !backend_.verifyStagedLog(*staged_, journal_))
      return result_(RESTORE_TX_STAGE_MISMATCH);
    if (!backend_.backupActiveLog(journal_)) { faulted_ = true; return result_(RESTORE_TX_BACKUP_FAILED); }
    if (!authorizationAlive_(authorization))
      return result_(RESTORE_TX_PRESENCE_LOST);
    if (!backend_.verifyActiveBackup(journal_)) { faulted_ = true; return result_(RESTORE_TX_BACKUP_VERIFY_FAILED); }
    if (!persistPhase_(RESTORE_TX_PHASE_BACKUP_READY))
      return result_(RESTORE_TX_JOURNAL_IO);
  }

  if (journal_.phase == RESTORE_TX_PHASE_BACKUP_READY) {
    if (!authorizationAlive_(authorization))
      return result_(RESTORE_TX_PRESENCE_LOST);
    if (!staged_ || !backend_.verifyStagedLog(*staged_, journal_))
      return result_(RESTORE_TX_STAGE_MISMATCH);
    if (!backend_.installStagedLog(*staged_, journal_)) { faulted_ = true; return result_(RESTORE_TX_INSTALL_FAILED); }
    if (!authorizationAlive_(authorization))
      return result_(RESTORE_TX_PRESENCE_LOST);
    if (!backend_.verifyInstalledLog(journal_)) { faulted_ = true; return result_(RESTORE_TX_INSTALL_VERIFY_FAILED); }
    if (!persistPhase_(RESTORE_TX_PHASE_LOG_INSTALLED))
      return result_(RESTORE_TX_JOURNAL_IO);
  }

  if (journal_.phase == RESTORE_TX_PHASE_LOG_INSTALLED) {
    if (!authorizationAlive_(authorization))
      return result_(RESTORE_TX_PRESENCE_LOST);
    if (!configSlots_.writeSlot(journal_.targetConfigSlot,
                                journal_.configSlot)) { faulted_ = true; return result_(RESTORE_TX_CONFIG_WRITE_FAILED); }
    uint8_t check[CFG_SLOT_SIZE];
    Config decoded;
    uint32_t seq = 0;
    const bool readOk = configSlots_.readSlot(journal_.targetConfigSlot, check);
    const bool valid = readOk && decodeConfig(check, decoded, seq) == CD_OK &&
        seq == journal_.configSeq &&
        memcmp(check, journal_.configSlot, sizeof check) == 0;
    secureClear(check, sizeof check);
    secureClear(&decoded, sizeof decoded);
    if (!valid) { faulted_ = true; return result_(RESTORE_TX_CONFIG_VERIFY_FAILED); }
    if (!persistPhase_(RESTORE_TX_PHASE_CONFIG_INSTALLED))
      return result_(RESTORE_TX_JOURNAL_IO);
  }

  if (journal_.phase == RESTORE_TX_PHASE_CONFIG_INSTALLED) {
    if (!backend_.verifyInstalledLog(journal_) ||
        !backend_.verifyCommittedPair(journal_)) { faulted_ = true; return result_(RESTORE_TX_PAIR_VERIFY_FAILED); }
    uint8_t check[CFG_SLOT_SIZE];
    Config decoded;
    uint32_t seq = 0;
    const bool readOk = configSlots_.readSlot(journal_.targetConfigSlot, check);
    const bool valid = readOk && decodeConfig(check, decoded, seq) == CD_OK &&
        seq == journal_.configSeq &&
        memcmp(check, journal_.configSlot, sizeof check) == 0;
    secureClear(check, sizeof check);
    secureClear(&decoded, sizeof decoded);
    if (!valid) { faulted_ = true; return result_(RESTORE_TX_CONFIG_VERIFY_FAILED); }
    if (!persistPhase_(RESTORE_TX_PHASE_COMMITTED))
      return result_(RESTORE_TX_JOURNAL_IO);
  }

  if (journal_.phase == RESTORE_TX_PHASE_COMMITTED)
    return result_(RESTORE_TX_AWAITING_ACK);
  return result_(RESTORE_TX_FAULTED);
}

RestoreTxResult RestoreTransactionCoordinator::finishAcknowledged_() {
  if (!hasJournal_ || journal_.phase != RESTORE_TX_PHASE_ACKNOWLEDGED)
    return result_(RESTORE_TX_NOT_READY);
  if (!backend_.cleanupRestoreArtifacts(journal_)) {
    faulted_ = true;
    return result_(RESTORE_TX_CLEANUP_FAILED);
  }
  if (!backend_.clearJournal(journal_)) {
    faulted_ = true;
    return result_(RESTORE_TX_CLEAR_JOURNAL_FAILED);
  }
  RestoreTxJournal check = {};
  bool present = false;
  bool ambiguous = false;
  const bool cleared = backend_.readJournal(check, present, ambiguous) &&
      !present && !ambiguous;
  secureClear(&check, sizeof check);
  if (!cleared) {
    faulted_ = true;
    return result_(RESTORE_TX_CLEAR_JOURNAL_FAILED);
  }
  const RestoreTxResult completed = {
      RESTORE_TX_RESTART_REQUIRED, RESTORE_TX_PHASE_ACKNOWLEDGED,
      journal_.records, journal_.lastSeq, journal_.configSeq};
  secureClear(&journal_, sizeof journal_);
  hasJournal_ = false;
  staged_ = nullptr;
  return completed;
}

RestoreTxResult RestoreTransactionCoordinator::acknowledge(
    IRestoreTransactionAuthorization& authorization) {
  if (faulted_) return result_(RESTORE_TX_FAULTED);
  if (!backend_.writable()) return result_(RESTORE_TX_NOT_READY);
  if (!loadJournal_()) return result_(loadError_);
  if (!hasJournal_ || journal_.phase != RESTORE_TX_PHASE_COMMITTED)
    return result_(RESTORE_TX_NOT_READY);
  if (!backend_.closeWritersAndDrain()) {
    faulted_ = true;
    return result_(RESTORE_TX_NOT_READY);
  }
  if (!authorization.authorizeAcknowledge(journal_.operationId,
                                          journal_.lastSeq))
    return result_(RESTORE_TX_AUTH_DENIED);
  if (!authorization.continueAuthorized())
    return result_(RESTORE_TX_PRESENCE_LOST);
  if (!persistPhase_(RESTORE_TX_PHASE_ACKNOWLEDGED))
    return result_(RESTORE_TX_JOURNAL_IO);
  return finishAcknowledged_();
}

}  // namespace att
