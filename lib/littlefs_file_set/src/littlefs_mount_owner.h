#pragma once

#include "fileset.h"
#include "littlefs_file_set.h"
#include "littlefs_serialized_fileset.h"

namespace att {

class ApplicationAttendanceGate;

class ILittleFsMountBackend {
 public:
  virtual ~ILittleFsMountBackend() {}
  // False by default: unknown mount implementations never authorize writes.
  virtual bool writable() const { return false; }
  // Must never format, erase, or repair storage.
  virtual bool mountWithoutFormatting(const char* mountpoint,
                                      const char* partitionLabel) = 0;
  // Unregister only a mount created by this backend instance.
  virtual bool unmountOwned() = 0;
};

enum LittleFsOwnerState {
  LITTLEFS_OWNER_NOT_STARTED = 0,
  LITTLEFS_OWNER_STARTING,
  LITTLEFS_OWNER_READY,
  LITTLEFS_OWNER_RECOVERY_READY,
  LITTLEFS_OWNER_FAULT,
  LITTLEFS_OWNER_RECOVERY_CLEANUP_FAILED,
  LITTLEFS_OWNER_NORMAL_CLEANUP_FAILED
};

class ILittleFsStorageOwner {
 public:
  virtual ~ILittleFsStorageOwner() {}
  virtual LittleFsOwnerState state() const = 0;
  // True only after a ready mount whose backend explicitly supports writes.
  virtual bool writable() const { return false; }
  // Unknown owners cannot prove that a FAULT state has no live mount. Recovery
  // must treat cleanup as unverified until the concrete owner says otherwise.
  virtual bool cleanupFailed() const { return true; }
  // Unknown owners cannot prove that they can relinquish an owned mount.
  virtual bool end() { return false; }
  virtual IFileSet* fileSet() = 0;
};

// Owns the mount-to-IFileSet boundary. Call begin() from the serialized
// application owner; it is idempotent while READY and may be called again
// after a successful end(). Storage is unavailable until mount and full
// read-only layout validation succeed.
class LittleFsStorageOwner final : public ILittleFsStorageOwner {
 public:
  LittleFsStorageOwner(ILittleFsMountBackend& backend,
                       const char* mountpoint,
                       const char* partitionLabel,
                       int16_t utcOffsetMin);

  LittleFsStorageOwner(const LittleFsStorageOwner&) = delete;
  LittleFsStorageOwner& operator=(const LittleFsStorageOwner&) = delete;

  bool begin();
  // Closes mutation/read access, waits for in-flight serialized I/O, and
  // unmounts this owner's normal mount. The caller must stop all producers;
  // reboot after external recovery mutations before reusing replay projections.
  bool end() override;
  bool cleanupFailed() const override { return cleanupFailed_; }
  // Mounts without formatting but deliberately skips normal log validation.
  // Use only in a dedicated recovery-service boot path; fileSet() stays hidden.
  bool beginRecovery();
  bool endRecovery();
  LittleFsOwnerState state() const override { return state_; }
  bool writable() const override;
  IFileSet* fileSet() override;

 private:
  enum {
    MOUNTPOINT_CAP = 32,     // Matches LittleFsFileSet mountpoint capacity.
    PARTITION_LABEL_CAP = 17 // esp_partition_t::label (16) plus NUL.
  };

  ILittleFsMountBackend& backend_;
  LittleFsFileSet files_;
  LittleFsStorageMutex storageMutex_;
  SerializedFileSet serializedFiles_;
  char mountpoint_[MOUNTPOINT_CAP];
  char partitionLabel_[PARTITION_LABEL_CAP];
  int16_t utcOffsetMin_;
  bool configValid_;
  bool cleanupFailed_;
  bool recoveryOnly_;
  LittleFsOwnerState state_;

  // The only caller is the application gate after config, storage, log,
  // roster, admission and closure replay have all been bound and verified.
  bool enableWritesAfterReplay_();
  friend class ApplicationAttendanceGate;
};

}  // namespace att
