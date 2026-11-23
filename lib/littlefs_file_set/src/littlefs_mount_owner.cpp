#include "littlefs_mount_owner.h"
#include "event_codec.h"

#include <string.h>

namespace att {
namespace {

bool copyBounded(char* destination, size_t capacity, const char* source) {
  if (!destination || !source || capacity == 0) return false;
  const size_t length = strlen(source);
  if (length == 0 || length >= capacity) return false;
  memcpy(destination, source, length + 1);
  return true;
}

}  // namespace

LittleFsStorageOwner::LittleFsStorageOwner(
    ILittleFsMountBackend& backend, const char* mountpoint,
    const char* partitionLabel, int16_t utcOffsetMin)
    : backend_(backend),
      files_(mountpoint, backend.writable()),
      storageMutex_(),
      serializedFiles_(files_, storageMutex_, false),
      mountpoint_{0},
      partitionLabel_{0},
      utcOffsetMin_(utcOffsetMin),
      configValid_(false),
      cleanupFailed_(false),
      recoveryOnly_(false),
      state_(LITTLEFS_OWNER_NOT_STARTED) {
  configValid_ = files_.valid() && storageMutex_.valid() &&
      utcOffsetMin_ >= -720 && utcOffsetMin_ <= 840 &&
      copyBounded(mountpoint_, sizeof mountpoint_, mountpoint) &&
      copyBounded(partitionLabel_, sizeof partitionLabel_, partitionLabel);
}

bool LittleFsStorageOwner::begin() {
  if (recoveryOnly_) return false;
  if (state_ == LITTLEFS_OWNER_READY) return true;
  if (state_ != LITTLEFS_OWNER_NOT_STARTED) return false;

  serializedFiles_.setEnabled(false);
  state_ = LITTLEFS_OWNER_STARTING;
  if (!configValid_ ||
      !backend_.mountWithoutFormatting(mountpoint_, partitionLabel_)) {
    state_ = LITTLEFS_OWNER_FAULT;
    return false;
  }

  // Validate the VFS root, canonical entries, record lengths, CRCs, types,
  // global sequence, and per-file commit boundaries. This is read-only.
  size_t existingFiles = 0;
  if (!files_.countCheckedWithFileSizeAlignment(REC_SIZE, existingFiles) ||
      !files_.validateRecordFiles(existingFiles, utcOffsetMin_)) {
    // The mount was created by this owner, but the data layout is not safe
    // to expose. Release only this backend's mount; never format or repair.
    cleanupFailed_ = !backend_.unmountOwned();
    state_ = cleanupFailed_ ? LITTLEFS_OWNER_NORMAL_CLEANUP_FAILED
                            : LITTLEFS_OWNER_FAULT;
    return false;
  }
  (void)existingFiles;

  state_ = LITTLEFS_OWNER_READY;
  serializedFiles_.setEnabled(true);
  return true;
}

bool LittleFsStorageOwner::end() {
  if (state_ != LITTLEFS_OWNER_READY &&
      state_ != LITTLEFS_OWNER_NORMAL_CLEANUP_FAILED)
    return false;

  // Hide the file set and close its gate before waiting on the mutex. Any
  // operation already holding the mutex completes; queued/stale handles wake
  // afterward, re-check the disabled gate, and never touch the unmounted VFS.
  // A failed unregister remains retryable, but never re-enables file access.
  state_ = LITTLEFS_OWNER_STARTING;
  serializedFiles_.setWritable(false);
  serializedFiles_.setEnabled(false);
  if (!storageMutex_.acquire()) {
    cleanupFailed_ = true;
    state_ = LITTLEFS_OWNER_NORMAL_CLEANUP_FAILED;
    return false;
  }
  const bool unmounted = backend_.unmountOwned();
  storageMutex_.release();
  if (!unmounted) {
    cleanupFailed_ = true;
    state_ = LITTLEFS_OWNER_NORMAL_CLEANUP_FAILED;
    return false;
  }
  cleanupFailed_ = false;
  state_ = LITTLEFS_OWNER_NOT_STARTED;
  return true;
}

bool LittleFsStorageOwner::beginRecovery() {
  if (!backend_.writable()) return false;
  if (state_ == LITTLEFS_OWNER_RECOVERY_READY) return true;
  if (state_ != LITTLEFS_OWNER_NOT_STARTED) return false;
  recoveryOnly_ = true;
  serializedFiles_.setWritable(false);
  serializedFiles_.setEnabled(false);
  state_ = LITTLEFS_OWNER_STARTING;
  if (!configValid_ ||
      !backend_.mountWithoutFormatting(mountpoint_, partitionLabel_)) {
    state_ = LITTLEFS_OWNER_FAULT;
    return false;
  }
  state_ = LITTLEFS_OWNER_RECOVERY_READY;
  return true;
}

bool LittleFsStorageOwner::endRecovery() {
  // A failed unregister must remain explicitly retryable. The normal file-set
  // gate stays disabled throughout; only this recovery owner may retry cleanup.
  if (!backend_.writable() ||
      (state_ != LITTLEFS_OWNER_RECOVERY_READY &&
       state_ != LITTLEFS_OWNER_RECOVERY_CLEANUP_FAILED))
    return false;
  state_ = LITTLEFS_OWNER_STARTING;
  serializedFiles_.setWritable(false);
  serializedFiles_.setEnabled(false);
  if (!storageMutex_.acquire()) {
    cleanupFailed_ = true;
    state_ = LITTLEFS_OWNER_RECOVERY_CLEANUP_FAILED;
    return false;
  }
  const bool unmounted = backend_.unmountOwned();
  storageMutex_.release();
  if (!unmounted) {
    cleanupFailed_ = true;
    state_ = LITTLEFS_OWNER_RECOVERY_CLEANUP_FAILED;
    return false;
  }
  cleanupFailed_ = false;
  state_ = LITTLEFS_OWNER_NOT_STARTED;
  return true;
}

bool LittleFsStorageOwner::writable() const {
  return state_ == LITTLEFS_OWNER_READY && backend_.writable() &&
      serializedFiles_.writable();
}

bool LittleFsStorageOwner::enableWritesAfterReplay_() {
  if (recoveryOnly_ || state_ != LITTLEFS_OWNER_READY || !backend_.writable() ||
      !serializedFiles_.enabled()) return false;
  serializedFiles_.setWritable(true);
  return serializedFiles_.writable();
}

IFileSet* LittleFsStorageOwner::fileSet() {
  return state_ == LITTLEFS_OWNER_READY ? &serializedFiles_ : nullptr;
}

}  // namespace att
