#pragma once

#include "closure_checkpoint_store.h"
#include "config_boot.h"
#include <nvs.h>

namespace att {

// Dedicated NVS checkpoint, separate from Config A/B slots. It is bound to the
// current immutable goLiveDay and local UTC offset. All methods are synchronous
// and belong to the serialized application loop. Any ambiguous write/readback
// latches this instance; reboot and explicit reconciliation are required.
class Esp32NvsClosureCheckpointStore : public IClosureCheckpointStore {
 public:
  explicit Esp32NvsClosureCheckpointStore(
      const ConfigBootCoordinator& configBoot);
  Esp32NvsClosureCheckpointStore(const Esp32NvsClosureCheckpointStore&) = delete;
  Esp32NvsClosureCheckpointStore& operator=(
      const Esp32NvsClosureCheckpointStore&) = delete;

  ClosureCheckpointStoreCode load(ClosureCheckpoint& out) override;
  // Create-only initialization. To avoid inventing a historical checkpoint,
  // this is accepted only on the immutable go-live local calendar day.
  ClosureCheckpointStoreCode initialize(uint32_t nowEpoch) override;
  // Monotonic replacement. A stale checkpoint is safe (it overstates GAP
  // coverage); a checkpoint may never be moved backwards or rebound to config.
  ClosureCheckpointStoreCode advance(uint32_t nowEpoch) override;

  bool faulted() const { return faulted_; }
  esp_err_t lastError() const { return lastError_; }

 private:
  bool ensureReady_();
  ClosureCheckpointStoreCode read_(ClosureCheckpoint& out);
  ClosureCheckpointStoreCode writeAndVerify_(uint32_t nowEpoch,
                                              bool createOnly);
  void latch_(esp_err_t error);

  const ConfigBootCoordinator& configBoot_;
  bool faulted_;
  esp_err_t lastError_;
};

}  // namespace att
