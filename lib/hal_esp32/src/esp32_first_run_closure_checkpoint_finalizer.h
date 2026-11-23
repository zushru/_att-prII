#pragma once

#include "setup_code_sequence.h"
#include "esp32_nvs_closure_checkpoint_store.h"

namespace att {

// Completes the durable half of first-run provisioning after Config A/B has
// been committed and reloaded. The checkpoint must use the exact final RTC
// readback that produced Config::goLiveDay.
class Esp32FirstRunClosureCheckpointFinalizer final
    : public IFirstRunProvisioningFinalizer {
 public:
  Esp32FirstRunClosureCheckpointFinalizer(
      const ConfigBootCoordinator& configBoot,
      Esp32NvsClosureCheckpointStore& checkpointStore);

  bool finalize(const Config& committedConfig,
                uint32_t confirmedUtcEpoch) override;

  bool finalized() const { return finalized_; }
  bool faulted() const { return faulted_; }

 private:
  const ConfigBootCoordinator& configBoot_;
  Esp32NvsClosureCheckpointStore& checkpointStore_;
  bool finalized_;
  bool faulted_;
};

}  // namespace att
