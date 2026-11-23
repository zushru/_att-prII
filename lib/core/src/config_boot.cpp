#include "config_boot.h"

#include <string.h>

namespace att {
namespace {

bool allZeroBytes(const uint8_t* value, size_t length) {
  if (value == nullptr) return true;
  uint8_t aggregate = 0;
  for (size_t i = 0; i < length; ++i) aggregate |= value[i];
  return aggregate == 0;
}

class ScopedWipe {
 public:
  ScopedWipe(void* value, size_t length)
      : bytes_(static_cast<volatile uint8_t*>(value)), length_(length) {}
  ScopedWipe(const ScopedWipe&) = delete;
  ScopedWipe& operator=(const ScopedWipe&) = delete;
  ~ScopedWipe() {
    for (size_t i = 0; i < length_; ++i) bytes_[i] = 0;
  }
 private:
  volatile uint8_t* bytes_;
  size_t length_;
};

}  // namespace

ConfigBootCoordinator::ConfigBootCoordinator(IConfigBootBackend& backend)
    : backend_(backend), state_(CFG_BOOT_COLD),
      selected_{CB_FAULT, -1, 0} {}

ConfigBootResult ConfigBootCoordinator::result_(ConfigBootCode code,
    ConfigProvisionCode provisionCode) const {
  return ConfigBootResult{code, state_, provisionCode};
}

void ConfigBootCoordinator::latchFault_() {
  state_ = CFG_BOOT_FAULT;
  selected_ = CfgPick{CB_FAULT, -1, 0};
}

ConfigBootResult ConfigBootCoordinator::start() {
  if (state_ != CFG_BOOT_COLD) return result_(CFG_BOOT_ALREADY_STARTED);

  // Latch fault first. Every successful branch below must explicitly establish
  // READY or NEEDS_SETUP; early exits can never leave a partly initialized state.
  latchFault_();
  if (!backend_.begin()) return result_(CFG_BOOT_BACKEND_BEGIN_FAILED);

  Config candidate;
  ScopedWipe wipeCandidate(&candidate, sizeof candidate);
  CfgPick picked = {CB_FAULT, -1, 0};
  if (!backend_.load(candidate, picked)) return result_(CFG_BOOT_LOAD_FAILED);

  if (picked.boot == CB_UNPROVISIONED && picked.active == -1 && picked.seq == 0) {
    selected_ = picked;
    state_ = CFG_BOOT_NEEDS_SETUP;
    return result_(CFG_BOOT_SETUP_REQUIRED);
  }

  if (picked.boot != CB_OK || (picked.active != 0 && picked.active != 1) ||
      picked.seq == 0 || !validateConfig(candidate)) {
    return result_(CFG_BOOT_INVALID_SELECTION);
  }

  config_ = candidate;
  selected_ = picked;
  state_ = CFG_BOOT_READY;
  return result_(CFG_BOOT_OK);
}

ConfigBootResult ConfigBootCoordinator::provision(const Config& initial,
    const IConfigProvisionGate& gate) {
  if (state_ != CFG_BOOT_NEEDS_SETUP)
    return result_(CFG_BOOT_NOT_SETUP_REQUIRED);

  ConfigProvisioner provisioner(backend_, gate);
  const ConfigProvisionResult written = provisioner.provision(initial, selected_);
  if (written.code == CFG_PROVISION_NOT_AUTHORIZED ||
      written.code == CFG_PROVISION_INVALID_CONFIG) {
    // These failures happen before any storage mutation and can be corrected by
    // the setup caller without rebooting.
    return result_(CFG_BOOT_PROVISION_REJECTED, written.code);
  }
  if (written.code != CFG_PROVISION_OK) {
    latchFault_();
    return result_(CFG_BOOT_PROVISION_FAILED, written.code);
  }

  // Do not trust the provisioner's returned selection as the boot state. Re-open
  // through the backend's canonical load path and compare the durable selection.
  Config reloaded;
  ScopedWipe wipeReloaded(&reloaded, sizeof reloaded);
  CfgPick picked = {CB_FAULT, -1, 0};
  if (!backend_.load(reloaded, picked)) {
    latchFault_();
    return result_(CFG_BOOT_RELOAD_FAILED, written.code);
  }
  if (picked.boot != CB_OK || picked.active != written.selected.active ||
      picked.seq != written.selected.seq || !validateConfig(reloaded) ||
      !configEqual(reloaded, initial)) {
    latchFault_();
    return result_(CFG_BOOT_RELOAD_FAILED, written.code);
  }

  config_ = reloaded;
  selected_ = picked;
  state_ = CFG_BOOT_READY;
  return result_(CFG_BOOT_OK, written.code);
}

ConfigAdminPinUpdateResult ConfigBootCoordinator::commitAdminPinCredential(
    const uint8_t salt[8], const uint8_t hash[32],
    IConfigApplyRuntime& runtime) {
  if (!ready() || selected_.boot != CB_OK ||
      (selected_.active != 0 && selected_.active != 1)) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_NOT_READY,
                                      selected_.seq};
  }
  const uint32_t currentGeneration = selected_.seq;
  if (salt == nullptr || hash == nullptr ||
      allZeroBytes(salt, sizeof config_.pinSalt) ||
      allZeroBytes(hash, sizeof config_.pinHash) ||
      memcmp(salt, config_.pinSalt, sizeof config_.pinSalt) == 0 ||
      memcmp(hash, config_.pinHash, sizeof config_.pinHash) == 0) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_INVALID,
                                      selected_.seq};
  }
  if (selected_.seq == UINT32_MAX) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_SEQUENCE_EXHAUSTED,
                                      selected_.seq};
  }

  Config next = config_;
  ScopedWipe wipeNext(&next, sizeof next);
  memcpy(next.pinSalt, salt, sizeof next.pinSalt);
  memcpy(next.pinHash, hash, sizeof next.pinHash);
  if (!validateConfig(next) || configEqual(next, config_)) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_INVALID,
                                      selected_.seq};
  }
  if (!runtime.canApply(next)) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_BUSY,
                                      selected_.seq};
  }

  // Re-read the canonical A/B selection before mutating storage. This prevents a
  // stale boot snapshot from overwriting a slot changed by another owner.
  Config persisted;
  ScopedWipe wipePersisted(&persisted, sizeof persisted);
  CfgPick persistedPick = {CB_FAULT, -1, 0};
  if (!backend_.load(persisted, persistedPick) ||
      persistedPick.boot != CB_OK ||
      persistedPick.active != selected_.active ||
      persistedPick.seq != selected_.seq ||
      !configEqual(persisted, config_)) {
    latchFault_();
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_VERIFY_FAILED,
                                      currentGeneration};
  }

  const uint32_t nextSeq = selected_.seq + 1;
  const uint8_t target = static_cast<uint8_t>(1 - selected_.active);
  uint8_t encoded[CFG_SLOT_SIZE] = {};
  ScopedWipe wipeEncoded(encoded, sizeof encoded);
  if (encodeConfig(next, nextSeq, encoded) == 0) {
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_INVALID,
                                      selected_.seq};
  }

  if (!backend_.writeSlot(target, encoded)) {
    latchFault_();
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_STORAGE_INDETERMINATE,
                                      currentGeneration};
  }

  // load() re-selects both slots and validates the provision marker. A partial,
  // stale, or malformed post-write state is never accepted as a successful PIN
  // change. Do not apply the in-RAM verifier until this check succeeds.
  Config committed;
  ScopedWipe wipeCommitted(&committed, sizeof committed);
  CfgPick committedPick = {CB_FAULT, -1, 0};
  if (!backend_.load(committed, committedPick) ||
      committedPick.boot != CB_OK || committedPick.active != target ||
      committedPick.seq != nextSeq || !configEqual(committed, next)) {
    latchFault_();
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_VERIFY_FAILED,
                                      currentGeneration};
  }

  if (!runtime.applyAfterCommit(committed)) {
    latchFault_();
    return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_RUNTIME_FAILED,
                                      currentGeneration};
  }

  config_ = committed;
  selected_ = committedPick;
  return ConfigAdminPinUpdateResult{CFG_ADMIN_PIN_UPDATE_OK, nextSeq};
}

bool ConfigBootCoordinator::selection(CfgPick& out) const {
  if (!ready()) return false;
  out = selected_;
  return true;
}

}  // namespace att
