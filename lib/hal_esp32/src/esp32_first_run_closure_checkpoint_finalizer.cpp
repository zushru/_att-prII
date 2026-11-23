#include "esp32_first_run_closure_checkpoint_finalizer.h"
#include "config_store.h"
#include "time_rules.h"

namespace att {

Esp32FirstRunClosureCheckpointFinalizer::
Esp32FirstRunClosureCheckpointFinalizer(
    const ConfigBootCoordinator& configBoot,
    Esp32NvsClosureCheckpointStore& checkpointStore)
    : configBoot_(configBoot), checkpointStore_(checkpointStore),
      finalized_(false), faulted_(false) {}

bool Esp32FirstRunClosureCheckpointFinalizer::finalize(
    const Config& committedConfig, uint32_t confirmedUtcEpoch) {
  if (finalized_ || faulted_) return false;

  const Config* selected = configBoot_.config();
  if (!configBoot_.ready() || selected == nullptr ||
      !validateConfig(committedConfig) || !configEqual(*selected, committedConfig) ||
      confirmedUtcEpoch < EPOCH_2020 ||
      dayNumber(confirmedUtcEpoch, committedConfig.params.utcOffsetMin) !=
          committedConfig.goLiveDay) {
    faulted_ = true;
    return false;
  }

  // The NVS adapter performs create-only checks, one atomic marker+blob commit,
  // and a fresh decode/readback. A failure is ambiguous and cannot be retried
  // through this object during the same boot.
  if (checkpointStore_.initialize(confirmedUtcEpoch) != CCS_OK) {
    faulted_ = true;
    return false;
  }

  ClosureCheckpoint persisted = {};
  if (checkpointStore_.load(persisted) != CCS_OK ||
      persisted.goLiveDay != committedConfig.goLiveDay ||
      persisted.lastTickEpoch != confirmedUtcEpoch) {
    faulted_ = true;
    return false;
  }

  finalized_ = true;
  return true;
}

}  // namespace att
