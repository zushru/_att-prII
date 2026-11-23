#pragma once

#include "littlefs_mount_owner.h"
#include <stddef.h>
#include <stdint.h>

namespace att {

static const char kSignedLittleFsPartitionLabel[] = "spiffs";
static const uint32_t kSignedLittleFsPartitionAddress = 0x290000UL;
static const uint32_t kSignedLittleFsPartitionSize = 0x170000UL;
static const uint32_t kSignedFlashCapacityBytes = 0x400000UL;

static inline bool isSignedFlashCapacity(uint32_t capacityBytes) {
  return capacityBytes == kSignedFlashCapacityBytes;
}

// A first-use format is permitted only when the entire partition is erased.
// A null/empty observation is not accepted as proof of a blank partition.
static inline bool isErasedPartitionBytes(const uint8_t* bytes,
                                          size_t length) {
  if (bytes == nullptr || length == 0) return false;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] != 0xFFU) return false;
  }
  return true;
}

// Refuse to mount a same-label partition from any other flash layout. The
// label alone is not sufficient when a stale or different partition table is
// present on the device.
static inline bool isSignedLittleFsPartitionGeometry(uint32_t address,
                                                      uint32_t size) {
  return address == kSignedLittleFsPartitionAddress &&
         size == kSignedLittleFsPartitionSize;
}

// ESP-IDF VFS backend. Registration is no-format and rejects an already
// mounted partition so a second owner cannot silently adopt another path.
class Esp32LittleFsMountBackend final : public ILittleFsMountBackend {
 public:
  // Defaults to a read-only VFS mount. The normal application may opt into a
  // write-capable VFS mount, but its serialized IFileSet remains read-only
  // until ApplicationAttendanceGate accepts the complete replay graph. The
  // isolated recovery composition uses a separate backend/owner.
  explicit Esp32LittleFsMountBackend(bool writable = false);
  Esp32LittleFsMountBackend(const Esp32LittleFsMountBackend&) = delete;
  Esp32LittleFsMountBackend& operator=(
      const Esp32LittleFsMountBackend&) = delete;
  Esp32LittleFsMountBackend(Esp32LittleFsMountBackend&&) = delete;
  Esp32LittleFsMountBackend& operator=(
      Esp32LittleFsMountBackend&&) = delete;

  // Read-only preflight used on both READY and NEEDS_SETUP boots.
  bool hasSignedPartitionGeometry() const;
  // Format only a fully erased signed partition. Existing/nonblank data is
  // never modified.
  bool initializeBlankPartitionIfErased();
  // Destructive maintenance-only format. Requires the signed geometry and an
  // unmounted partition; callers must have closed the storage owner first.
  bool formatSignedPartition();
  // Reports VFS capability, not application write authorization. A writable
  // backend alone never opens the LittleFsStorageOwner serialized gate.
  bool writable() const override { return writable_; }
  bool mountWithoutFormatting(const char* mountpoint,
                              const char* partitionLabel) override;
  bool unmountOwned() override;

 private:
  bool mounted_;
  bool writable_;
  char partitionLabel_[17];
};

}  // namespace att
