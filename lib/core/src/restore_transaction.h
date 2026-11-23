#pragma once

#include "backup_format.h"
#include "config_apply.h"
#include "pin_auth_session.h"
#include "restore_replay.h"
#include <stddef.h>
#include <stdint.h>

namespace att {

static const uint32_t RESTORE_TX_VERSION = 2;
static const size_t RESTORE_TX_OPERATION_ID_BYTES = 16;
static const size_t RESTORE_TX_MANIFEST_SHA256_BYTES = 32;
static const size_t RESTORE_TX_JOURNAL_BYTES = 280;

enum RestoreTxPhase : uint8_t {
  RESTORE_TX_PHASE_NONE = 0,
  RESTORE_TX_PHASE_PREPARED = 1,
  RESTORE_TX_PHASE_BACKUP_READY = 2,
  RESTORE_TX_PHASE_LOG_INSTALLED = 3,
  RESTORE_TX_PHASE_CONFIG_INSTALLED = 4,
  RESTORE_TX_PHASE_COMMITTED = 5,
  RESTORE_TX_PHASE_ACKNOWLEDGED = 6
};

// Canonical write-ahead record. Backends must serialize fields explicitly (not
// memcpy this C++ object), atomically publish one complete version, and read it
// back before reporting success. configSlot contains credentials and therefore
// must be stored only in protected device storage.
struct RestoreTxJournal {
  uint32_t version;
  uint8_t phase;
  uint8_t targetConfigSlot;
  uint16_t reserved;
  uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES];
  uint32_t records;
  uint32_t lastSeq;
  uint32_t recordsCrc32;
  uint32_t configSeq;
  uint8_t configSlot[CFG_SLOT_SIZE];
  uint32_t priorLogFileCount;
  uint64_t priorLogTotalBytes;
  uint8_t priorLogManifestSha256[RESTORE_TX_MANIFEST_SHA256_BYTES];
  uint32_t journalCrc32;
};

uint32_t calculateRestoreTxJournalCrc32(const RestoreTxJournal& journal);
bool validateRestoreTxJournal(const RestoreTxJournal& journal);
// Stable little-endian wire encoding used by durable journals. Never persist
// the in-memory C++ struct directly because padding/layout is toolchain-specific.
bool encodeRestoreTxJournal(const RestoreTxJournal& journal,
                            uint8_t out[RESTORE_TX_JOURNAL_BYTES]);
bool decodeRestoreTxJournal(const uint8_t* bytes, size_t length,
                            RestoreTxJournal& out);
bool generateRestoreTxOperationId(
    ISecureRandomSource& random,
    uint8_t out[RESTORE_TX_OPERATION_ID_BYTES]);

class IRestoreTransactionAuthorization {
 public:
  virtual ~IRestoreTransactionAuthorization() {}
  // Must bind approval to this exact operation and restored last sequence,
  // require an authenticated ADMIN, typed confirmation and local presence.
  virtual bool authorizeStart(
      const uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES],
      uint32_t restoredLastSeq) = 0;
  virtual bool authorizeResume(
      const uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES],
      uint32_t restoredLastSeq) = 0;
  virtual bool authorizeAcknowledge(
      const uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES],
      uint32_t restoredLastSeq) = 0;
  // Must remain true during each bounded backend operation.
  virtual bool continueAuthorized() const = 0;
};

class IRestoreTransactionJournalStore {
 public:
  virtual ~IRestoreTransactionJournalStore() {}
  virtual bool readJournal(RestoreTxJournal& out, bool& present,
                           bool& ambiguous) = 0;
  virtual bool writeJournal(const RestoreTxJournal& journal) = 0;
  virtual bool clearJournal(const RestoreTxJournal& expected) = 0;
};

class IRestoreTransactionBackend : public IRestoreTransactionJournalStore {
 public:
  virtual ~IRestoreTransactionBackend() {}
  virtual bool writable() const = 0;
  // Close every live writer and drain in-flight storage work. This remains in
  // force after a transaction begins; normal application startup must also
  // inspect the journal before opening any writable runtime.
  virtual bool closeWritersAndDrain() = 0;

  // The transaction journal must be a protected, independently durable record
  // (normally NVS), not stored inside the log directory being replaced.

  // Every operation below is idempotent for the exact operationId + manifest.
  // The stage is immutable from validation through acknowledgement.
  // On reboot, returns a handle to the same immutable staged stream. The
  // returned object is backend-owned and must outlive the coordinator.
  virtual IStorage* openStagedLog(const RestoreTxJournal& journal) = 0;
  virtual bool verifyStagedLog(
      IStorage& staged, const RestoreTxJournal& journal) = 0;
  // Capture a canonical SHA-256 inventory of the current live log while writers
  // are closed. Must be read-only; zero files are allowed but the digest must
  // still be SHA-256 of a defined empty-inventory encoding.
  virtual bool capturePriorLogManifest(RestoreTxJournal& journal) = 0;
  // Copy and durably verify the pre-restore active log against the exact
  // priorLog* manifest in the journal. Never change active data.
  virtual bool backupActiveLog(const RestoreTxJournal& journal) = 0;
  virtual bool verifyActiveBackup(const RestoreTxJournal& journal) = 0;
  // Promote the exact staged log while preserving the verified pre-restore copy.
  // Must tolerate power loss at any instruction and be safe to repeat.
  virtual bool installStagedLog(
      IStorage& staged, const RestoreTxJournal& journal) = 0;
  virtual bool verifyInstalledLog(const RestoreTxJournal& journal) = 0;
  // Verify that the active log and selected config slot are exactly this restore.
  virtual bool verifyCommittedPair(const RestoreTxJournal& journal) = 0;
  // Only called after explicit acknowledgement. Idempotently remove stage,
  // backup and temporary artifacts, but never mutate the active restored data.
  virtual bool cleanupRestoreArtifacts(const RestoreTxJournal& journal) = 0;
};

enum RestoreTxCode : uint8_t {
  RESTORE_TX_NO_PENDING = 0,
  RESTORE_TX_AWAITING_ACK,
  RESTORE_TX_RESTART_REQUIRED,
  RESTORE_TX_NOT_READY,
  RESTORE_TX_AUTH_DENIED,
  RESTORE_TX_PRESENCE_LOST,
  RESTORE_TX_INVALID_EVIDENCE,
  RESTORE_TX_STAGE_MISMATCH,
  RESTORE_TX_CONFIG_FAULT,
  RESTORE_TX_SEQUENCE_EXHAUSTED,
  RESTORE_TX_JOURNAL_IO,
  RESTORE_TX_JOURNAL_AMBIGUOUS,
  RESTORE_TX_JOURNAL_CORRUPT,
  RESTORE_TX_BACKUP_FAILED,
  RESTORE_TX_BACKUP_VERIFY_FAILED,
  RESTORE_TX_INSTALL_FAILED,
  RESTORE_TX_INSTALL_VERIFY_FAILED,
  RESTORE_TX_CONFIG_WRITE_FAILED,
  RESTORE_TX_CONFIG_VERIFY_FAILED,
  RESTORE_TX_PAIR_VERIFY_FAILED,
  RESTORE_TX_CLEANUP_FAILED,
  RESTORE_TX_CLEAR_JOURNAL_FAILED,
  RESTORE_TX_FAULTED
};

struct RestoreTxResult {
  RestoreTxCode code;
  RestoreTxPhase phase;
  uint32_t records;
  uint32_t lastSeq;
  uint32_t configSeq;
};

// Write-ahead restore transaction. The previous log is backed up before the
// active log changes. The staged log is promoted before the new config slot is
// committed. A valid journal causes boot to remain read-only until roll-forward
// reaches COMMITTED; the old log is retained until explicit acknowledgement.
// Any ambiguous I/O latches this instance. Construct a new instance after reboot
// and call recoverPending() before normal writable application startup.
class RestoreTransactionCoordinator {
 public:
  RestoreTransactionCoordinator(IRestoreTransactionBackend& backend,
                                IConfigSlotBackend& configSlots)
      : backend_(backend), configSlots_(configSlots), faulted_(false),
        journalLoaded_(false), hasJournal_(false), staged_(nullptr),
        journal_{}, loadError_(RESTORE_TX_JOURNAL_IO) {}
  ~RestoreTransactionCoordinator();
  RestoreTransactionCoordinator(const RestoreTransactionCoordinator&) = delete;
  RestoreTransactionCoordinator& operator=(const RestoreTransactionCoordinator&) = delete;

  // Requires a fully completed outer AEAD container whose plaintext sink is
  // this exact validator, then performs semantic replay itself. Callers cannot
  // pair a completed unrelated reader with separately staged plaintext.
  // operationId must be fresh, random and non-zero.
  RestoreTxResult begin(BackupReader& outerReader, RestoreValidator& validator,
                        RestoreReplayAudit& replay,
                        const uint8_t operationId[RESTORE_TX_OPERATION_ID_BYTES],
                        IRestoreTransactionAuthorization& authorization);

  // Call before normal app/storage startup. If a transaction is pending, it
  // closes writers, validates the journal and requires fresh local approval
  // before resuming any destructive phase. COMMITTED is read-only and returns
  // AWAITING_ACK without modifying storage.
  RestoreTxResult recoverPending(
      IRestoreTransactionAuthorization& authorization);

  // Explicitly discard the retained pre-restore backup only after an ADMIN has
  // reviewed the restored device and approved this exact operation/sequence.
  // Completion always requires a reboot before normal writes resume.
  RestoreTxResult acknowledge(
      IRestoreTransactionAuthorization& authorization);

  bool faulted() const { return faulted_; }
  bool pending() const { return hasJournal_; }
  const RestoreTxJournal& journal() const { return journal_; }

 private:
  RestoreTxResult result_(RestoreTxCode code) const;
  bool loadJournal_();
  bool persistPhase_(RestoreTxPhase phase);
  bool authorizationAlive_(IRestoreTransactionAuthorization* authorization) const;
  RestoreTxResult resume_(IRestoreTransactionAuthorization* authorization);
  RestoreTxResult finishAcknowledged_();
  void wipe_();

  IRestoreTransactionBackend& backend_;
  IConfigSlotBackend& configSlots_;
  bool faulted_;
  bool journalLoaded_;
  bool hasJournal_;
  IStorage* staged_;
  RestoreTxJournal journal_;
  RestoreTxCode loadError_;
};

}  // namespace att
