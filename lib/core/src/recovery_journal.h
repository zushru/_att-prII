#pragma once

#include "month_set.h"
#include "log_store.h"
#include "pin_auth_session.h"
#include <stddef.h>
#include <stdint.h>

namespace att {

static const uint32_t RECOVERY_JOURNAL_VERSION = 1;
static const size_t RECOVERY_MAX_FILES = 64;
static const size_t RECOVERY_OPERATION_ID_BYTES = 16;

// Uses the project's lifecycle-gated secure random provider. A zero ID or an
// unavailable entropy source is rejected rather than replaced with a counter.
bool generateRecoveryOperationId(
    ISecureRandomSource& random,
    uint8_t out[RECOVERY_OPERATION_ID_BYTES]);

class IRecoveryAuthorization {
 public:
  virtual ~IRecoveryAuthorization() {}
  // Implementations must require one authenticated ADMIN, fresh typed
  // confirmation bound to operationId/retainedBytes, and local service presence.
  virtual bool authorize(const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
                         uint64_t retainedBytes) = 0;
  // Must remain true while the local service-presence control is held. The
  // journal samples it before each durable phase and each month-file change.
  virtual bool continueAuthorized() const = 0;
};

struct RecoveryPlan {
  uint32_t version;
  uint8_t operationId[RECOVERY_OPERATION_ID_BYTES];
  size_t sourceCount;
  uint64_t sourceTotalBytes;
  uint64_t retainedBytes;
  uint32_t sourceManifestCrc32;
  uint8_t sourceManifestSha256[32];
  uint32_t planCrc32;
  MonthFileInventoryEntry source[RECOVERY_MAX_FILES];
};

enum RecoveryStatus {
  RECOVERY_COMPLETE = 0,
  RECOVERY_NO_PENDING,
  RECOVERY_AUTH_REQUIRED,
  RECOVERY_READ_ONLY,
  RECOVERY_INVALID_REQUEST,
  RECOVERY_AMBIGUOUS_PENDING,
  RECOVERY_PENDING_EXISTS,
  RECOVERY_BACKEND_FAILURE,
  RECOVERY_SOURCE_CHANGED,
  RECOVERY_ARCHIVE_FAILURE,
  RECOVERY_ACTIVE_MISMATCH,
  RECOVERY_PREVIEW_READY,
  RECOVERY_STALE_PREVIEW,
  RECOVERY_PRESENCE_REQUIRED
};

// Read-only challenge material. The client displays the operation ID and
// derived retained-byte count, then returns this exact preview with the
// typed confirmation. prepare() always re-derives and compares it.
struct RecoveryPreview {
  RecoveryStatus status;
  uint8_t operationId[RECOVERY_OPERATION_ID_BYTES];
  size_t sourceCount;
  uint64_t sourceTotalBytes;
  uint64_t retainedBytes;
  uint8_t sourceManifestSha256[32];
  uint32_t planCrc32;
};

// Retains exactly one service-issued preview and consumes it on any prepare/
// resume attempt. This binds the externally returned challenge to server-side
// state; clients cannot substitute an arbitrary operation ID or digest.
class RecoveryPreviewGate {
 public:
  RecoveryPreviewGate();
  void issueInitial(const RecoveryPreview& preview);
  void issuePending(const RecoveryPreview& preview);
  bool consumeInitial(const RecoveryPreview& preview);
  bool consumePending(const RecoveryPreview& preview);
  void clear();

 private:
  enum Kind : uint8_t { KIND_NONE = 0, KIND_INITIAL = 1, KIND_PENDING = 2 };
  bool consume_(const RecoveryPreview& preview, Kind expected);
  bool matches_(const RecoveryPreview& left,
                const RecoveryPreview& right) const;
  RecoveryPreview issued_;
  Kind kind_;
};

struct RecoveryResult {
  RecoveryStatus status;
  uint8_t operationId[RECOVERY_OPERATION_ID_BYTES];
  uint64_t retainedBytes;
  size_t filesArchived;
  size_t filesChanged;
};

class IRecoveryBackend {
 public:
  virtual ~IRecoveryBackend() {}

  virtual bool writable() const = 0;
  // Checks current free space for an additional archive payload. The backend
  // must include a conservative reserve for metadata, temp files and GC slack.
  virtual bool capacityAvailable(const RecoveryPlan& plan,
                                 uint64_t additionalDataBytes) = 0;

  // Intent and phase markers must be immutable and atomically published.
  // A failed publication must leave the active /log source untouched.
  virtual bool writeIntent(const RecoveryPlan& plan) = 0;
  virtual bool findPending(RecoveryPlan& plan, bool& found,
                           bool& ambiguous) = 0;

  // ensureArchive() is idempotent and may replace an incomplete staging file
  // only while no valid archive-ready marker exists. verifyArchive() compares
  // exact length and bytes against the plan's recorded source fingerprint.
  virtual bool ensureArchive(const RecoveryPlan& plan, size_t index) = 0;
  // Real backends override this to sample the continuing local-presence lease
  // between bounded copy chunks. The journal checks the lease before/after.
  virtual bool ensureArchiveWithLease(
      const RecoveryPlan& plan, size_t index,
      IRecoveryAuthorization& authorization) {
    (void)authorization;
    return ensureArchive(plan, index);
  }
  virtual bool verifyArchive(const RecoveryPlan& plan, size_t index) = 0;
  virtual bool archiveReady(const RecoveryPlan& plan, bool& ready) = 0;
  virtual bool writeArchiveReady(const RecoveryPlan& plan) = 0;

  // Active operations must be idempotent. A missing file is distinguishable
  // from a metadata/read error. Prefix comparison is against the immutable
  // staged original, never a CRC-only comparison.
  virtual bool activeFileState(uint32_t monthKey, bool& exists,
                               uint64_t& size) = 0;
  virtual bool activeMatchesArchivePrefix(const RecoveryPlan& plan,
                                          size_t index,
                                          uint64_t prefixBytes) = 0;
  // Rebuilds one active month from the verified archive. It must be safe to
  // repeat after interruption at any point; the archive is never modified.
  virtual bool replaceActiveFromArchive(const RecoveryPlan& plan, size_t index,
                                        uint64_t retainedFileBytes) = 0;
  virtual bool replaceActiveFromArchiveWithLease(
      const RecoveryPlan& plan, size_t index, uint64_t retainedFileBytes,
      IRecoveryAuthorization& authorization) {
    (void)authorization;
    return replaceActiveFromArchive(plan, index, retainedFileBytes);
  }
  virtual bool removeActiveFile(const RecoveryPlan& plan, size_t index) = 0;
  virtual bool verifyActiveResult(const RecoveryPlan& plan) = 0;

  virtual bool completeMarker(const RecoveryPlan& plan, bool& complete) = 0;
  virtual bool writeCompleteMarker(const RecoveryPlan& plan) = 0;
};

// Archive-first recovery. A durable intent is published before any active
// month file is changed. All originals are copied and byte-verified before
// the archive-ready marker is published. Only then may the active suffix be
// trimmed. resumePending() repeats each step idempotently after reboot and
// requires a fresh authorization decision before any mutation.
class RecoveryJournal {
 public:
  explicit RecoveryJournal(IRecoveryBackend& backend) : backend_(backend) {}

  // Preview performs full structural, semantic and inventory validation but
  // writes nothing and does not request authorization.
  RecoveryPreview preview(
      const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
      MonthSet& source, int16_t utcOffsetMin);

  // prepare() re-derives the plan and rejects a stale/altered preview before
  // requesting authorization or publishing the durable intent.
  RecoveryResult prepare(const RecoveryPreview& preview, MonthSet& source,
                         int16_t utcOffsetMin,
                         IRecoveryAuthorization& authorization);

  // Read-only challenge for an existing interrupted transaction.
  RecoveryPreview pendingPreview();
  RecoveryResult resumePending(const RecoveryPreview& preview,
                               IRecoveryAuthorization& authorization);

  static bool validatePlan(const RecoveryPlan& plan);
  static uint32_t calculatePlanCrc32(const RecoveryPlan& plan);

 private:
  RecoveryStatus derivePlan_(
      const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
      MonthSet& source, int16_t utcOffsetMin, RecoveryPlan& plan);
  RecoveryResult prepare_(const RecoveryPreview& preview, MonthSet& source,
                          int16_t utcOffsetMin,
                          IRecoveryAuthorization& authorization);
  RecoveryResult resumePending_(const RecoveryPreview& preview,
                                IRecoveryAuthorization& authorization);
  RecoveryResult resume_(const RecoveryPlan& plan,
                         IRecoveryAuthorization& authorization);
  RecoveryResult result_(RecoveryStatus status,
                         const RecoveryPlan* plan = nullptr) const;
  IRecoveryBackend& backend_;
};

}  // namespace att
