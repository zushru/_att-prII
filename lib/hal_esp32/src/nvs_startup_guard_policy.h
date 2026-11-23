#pragma once

#include <stddef.h>
#include <stdint.h>

namespace att {

// Explicit whole-partition nvs_flash_erase* APIs are rejected separately at
// the linker boundary on every boot. This lower-level rule always rejects an
// exact DATA_NVS erase and, after a failed first init, every non-empty range.
// It prevents split framework-recovery erases without blocking sector erases
// required by ordinary NVS key updates after a successful startup. It does not
// cover raw esp_flash APIs or deliberate chunking through those lower layers.
inline bool shouldBlockNvsPartitionErase(bool isNvsDataPartition,
                                          bool firstInitFailed,
                                          size_t offset,
                                          size_t eraseSize,
                                          size_t partitionSize) {
  if (!isNvsDataPartition || eraseSize == 0) return false;
  const bool exactWholePartition = partitionSize > 0 && offset == 0 &&
                                   eraseSize == partitionSize;
  return exactWholePartition || firstInitFailed;
}

// The application may use NVS only after exactly one pre-setup init call,
// that call succeeded, and no DATA_NVS erase was intercepted.
inline bool mayUseNvsAfterStartup(uint32_t initCallCount,
                                  bool firstInitSucceeded,
                                  bool nvsEraseBlocked) {
  // Arduino-ESP32 3.3.12 calls nvs_flash_init() once in initArduino(). Any
  // missing or additional pre-setup call makes the captured startup result
  // ambiguous and therefore fails closed.
  return initCallCount == 1 && firstInitSucceeded &&
         !nvsEraseBlocked;
}

}  // namespace att
