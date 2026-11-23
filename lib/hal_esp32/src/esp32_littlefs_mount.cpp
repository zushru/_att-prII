#include "esp32_littlefs_mount.h"

// This backend is compiled only by the ESP-IDF target. The HAL library is
// also built by native suites, where ESP-IDF VFS headers do not exist.
#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <esp_vfs.h>
#include <string.h>

namespace att {
namespace {

const esp_partition_t* findSignedLittleFsPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_ANY,
                                  kSignedLittleFsPartitionLabel);
}

bool isMountableSignedLittleFsPartition(const esp_partition_t* partition) {
  return partition != nullptr &&
      partition->type == ESP_PARTITION_TYPE_DATA &&
      partition->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS &&
      isSignedLittleFsPartitionGeometry(partition->address, partition->size) &&
      !partition->readonly && !partition->encrypted;
}

}  // namespace

bool Esp32LittleFsMountBackend::hasSignedPartitionGeometry() const {
  // The final SPIFFS partition ends exactly at 4 MiB. A table entry with the
  // expected address/size is not sufficient if the physical chip is smaller.
  return isSignedFlashCapacity(ESP.getFlashChipSize()) &&
      isMountableSignedLittleFsPartition(findSignedLittleFsPartition());
}

bool Esp32LittleFsMountBackend::initializeBlankPartitionIfErased() {
  if (writable_ || !hasSignedPartitionGeometry() || mounted_ ||
      esp_littlefs_mounted(kSignedLittleFsPartitionLabel))
    return false;

  const esp_partition_t* partition = findSignedLittleFsPartition();
  if (!isMountableSignedLittleFsPartition(partition)) return false;

  uint8_t block[256];
  uint32_t offset = 0;
  while (offset < partition->size) {
    const size_t count = partition->size - offset < sizeof block
        ? static_cast<size_t>(partition->size - offset) : sizeof block;
    if (esp_partition_read(partition, offset, block, count) != ESP_OK)
      return false;

    // Any non-erased byte means this partition may contain a filesystem or
    // valuable data. Leave it untouched and let normal no-format mount decide.
    if (!isErasedPartitionBytes(block, count)) return true;
    offset += static_cast<uint32_t>(count);
  }

  // The entire exact signed partition was read and every byte was 0xFF.
  // Formatting cannot discard an existing file because none is present.
  if (esp_littlefs_format(kSignedLittleFsPartitionLabel) != ESP_OK)
    return false;
  return true;
}

bool Esp32LittleFsMountBackend::formatSignedPartition() {
  if (mounted_ || !hasSignedPartitionGeometry() ||
      esp_littlefs_mounted(kSignedLittleFsPartitionLabel))
    return false;
  const esp_partition_t* partition = findSignedLittleFsPartition();
  if (!isMountableSignedLittleFsPartition(partition)) return false;
  // This is the only deliberate destructive path for the attendance partition.
  // Keep the fixed signed geometry check immediately adjacent to the format.
  return esp_littlefs_format(kSignedLittleFsPartitionLabel) == ESP_OK;
}

Esp32LittleFsMountBackend::Esp32LittleFsMountBackend(bool writable)
    : mounted_(false), writable_(writable), partitionLabel_{0} {}

bool Esp32LittleFsMountBackend::mountWithoutFormatting(
    const char* mountpoint, const char* partitionLabel) {
  if (!mountpoint || mountpoint[0] != '/' || !partitionLabel ||
      partitionLabel[0] == '\0')
    return false;

  const size_t pathLength = strlen(mountpoint);
  if (pathLength < 2 || pathLength > ESP_VFS_PATH_MAX ||
      mountpoint[pathLength - 1] == '/')
    return false;

  const size_t labelLength = strlen(partitionLabel);
  if (labelLength >= sizeof(((esp_partition_t*)0)->label)) return false;

  const esp_partition_t* partition = findSignedLittleFsPartition();
  if (!isSignedFlashCapacity(ESP.getFlashChipSize()) ||
      strcmp(partitionLabel, kSignedLittleFsPartitionLabel) != 0 ||
      !isMountableSignedLittleFsPartition(partition))
    return false;

  // Do not accept an existing mount: its base path may belong to another
  // owner. Registration returns INVALID_STATE on duplicate partition/path.
  if (esp_littlefs_mounted(partitionLabel)) return false;

  esp_vfs_littlefs_conf_t config = {};
  config.base_path = mountpoint;
  config.partition_label = partitionLabel;
  config.partition = nullptr;
  config.format_if_mount_failed = false;
  // VFS capability is separate from IFileSet authorization. The normal
  // application may mount with write support so the explicitly gated clock
  // audit transaction can append; all ordinary callers still see a read-only
  // SerializedFileSet until ApplicationAttendanceGate opens its private gate.
  config.read_only = true;
  if (writable_) config.read_only = false;
  config.dont_mount = false;
  config.grow_on_mount = false;

  if (esp_vfs_littlefs_register(&config) != ESP_OK) return false;
  memcpy(partitionLabel_, partitionLabel, labelLength + 1);
  mounted_ = true;
  return true;
}

bool Esp32LittleFsMountBackend::unmountOwned() {
  if (!mounted_ || partitionLabel_[0] == '\0') return false;
  if (esp_vfs_littlefs_unregister(partitionLabel_) != ESP_OK) return false;
  mounted_ = false;
  partitionLabel_[0] = '\0';
  return true;
}

}  // namespace att

#endif  // ESP_PLATFORM
