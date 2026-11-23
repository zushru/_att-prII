#pragma once

#include "esp32_littlefs_mount.h"
#include "littlefs_mount_owner.h"
#include "admin_recovery_authorization.h"
#include "littlefs_file_set.h"
#include "littlefs_recovery_backend.h"
#include "recovery_journal.h"

namespace att {

// Dedicated, no-format maintenance service. It must run with the attendance
// application stopped and no competing filesystem tasks. The normal owner
// never receives this writable mount or its backend.
class Esp32RecoveryService final {
 public:
  Esp32RecoveryService(ILittleFsStorageOwner& normalStorageOwner,
                       const char* mountpoint, const char* partitionLabel,
                       int16_t utcOffsetMin,
                       ISecureRandomSource& random);
  ~Esp32RecoveryService();

  Esp32RecoveryService(const Esp32RecoveryService&) = delete;
  Esp32RecoveryService& operator=(const Esp32RecoveryService&) = delete;

  // Writable recovery mount is opened only while the injected, local GPIO
  // service-presence control is currently asserted. The exact object is kept
  // for subsequent preview/operation checks.
  bool begin(IRecoveryPhysicalPresence& presence);
  RecoveryPreview preview();
  RecoveryResult prepare(const RecoveryPreview& preview,
                         AdminRecoveryAuthorization& authorization);
  RecoveryPreview pendingPreview();
  RecoveryResult resumePending(const RecoveryPreview& preview,
                               AdminRecoveryAuthorization& authorization);
  bool end();
  bool active() const { return active_ && !cleanupOnly_; }
  bool cleanupPending() const {
    return normalOwnerCleanupPending_ || (active_ && cleanupOnly_);
  }
  // Once normal storage has been relinquished, this process must not resume
  // cached LogStore/Roster/Closure projections. Reboot after cleanup (or any
  // failed handoff) and reconstruct them from the selected on-disk state.
  bool restartRequired() const { return restartRequired_; }

 private:
  ILittleFsStorageOwner& normalStorageOwner_;
  Esp32LittleFsMountBackend mountBackend_;
  LittleFsStorageOwner storageOwner_;
  LittleFsFileSet sourceFiles_;
  MonthSet sourceMonths_;
  LittleFsRecoveryBackend recoveryBackend_;
  RecoveryJournal journal_;
  RecoveryPreviewGate previewGate_;
  ISecureRandomSource& random_;
  IRecoveryPhysicalPresence* presence_;
  int16_t utcOffsetMin_;
  bool active_;
  bool cleanupOnly_;
  bool normalOwnerCleanupPending_;
  bool restartRequired_;

  RecoveryResult inactiveResult_() const;
  void revokePresence_();
};

}  // namespace att
