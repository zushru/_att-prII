#pragma once
#include "config_apply.h"
#include "config_provision.h"

namespace att {

// The production boot path must not construct runtime services from defaults when
// NVS is blank or unhealthy. This interface combines the slot backend with the
// explicit begin/load lifecycle provided by the platform adapter.
class IConfigBootBackend : public IConfigProvisionBackend {
 public:
  virtual bool begin() = 0;
  virtual bool load(Config& out, CfgPick& pick) = 0;
};

enum ConfigBootState : uint8_t {
  CFG_BOOT_COLD,
  CFG_BOOT_READY,
  CFG_BOOT_NEEDS_SETUP,
  CFG_BOOT_FAULT
};

enum ConfigBootCode : uint8_t {
  CFG_BOOT_OK,
  CFG_BOOT_SETUP_REQUIRED,
  CFG_BOOT_ALREADY_STARTED,
  CFG_BOOT_BACKEND_BEGIN_FAILED,
  CFG_BOOT_LOAD_FAILED,
  CFG_BOOT_INVALID_SELECTION,
  CFG_BOOT_NOT_SETUP_REQUIRED,
  CFG_BOOT_PROVISION_REJECTED,
  CFG_BOOT_PROVISION_FAILED,
  CFG_BOOT_RELOAD_FAILED
};

struct ConfigBootResult {
  ConfigBootCode code;
  ConfigBootState state;
  ConfigProvisionCode provisionCode;  // meaningful only for provision results
};

enum ConfigAdminPinUpdateCode : uint8_t {
  CFG_ADMIN_PIN_UPDATE_OK,
  CFG_ADMIN_PIN_UPDATE_NOT_READY,
  CFG_ADMIN_PIN_UPDATE_INVALID,
  CFG_ADMIN_PIN_UPDATE_BUSY,
  CFG_ADMIN_PIN_UPDATE_SEQUENCE_EXHAUSTED,
  CFG_ADMIN_PIN_UPDATE_VERIFY_FAILED,
  CFG_ADMIN_PIN_UPDATE_STORAGE_INDETERMINATE,
  CFG_ADMIN_PIN_UPDATE_RUNTIME_FAILED
};

struct ConfigAdminPinUpdateResult {
  ConfigAdminPinUpdateCode code;
  uint32_t generation;
};

// One-shot boot/provision coordinator. It is deliberately constructed before the
// rest of the application runtime. Runtime services may be constructed only after
// state() == CFG_BOOT_READY and config()/selection() are valid.
//
// All calls are serialized on the main loop. Any storage/verification ambiguity
// latches CFG_BOOT_FAULT for this boot; reboot and a fresh backend load are required.
class ConfigBootCoordinator {
 public:
  explicit ConfigBootCoordinator(IConfigBootBackend& backend);
  ConfigBootCoordinator(const ConfigBootCoordinator&) = delete;
  ConfigBootCoordinator& operator=(const ConfigBootCoordinator&) = delete;

  ConfigBootResult start();
  ConfigBootResult provision(const Config& initial, const IConfigProvisionGate& gate);

  // Credential-only A/B transaction. It updates only the PIN salt/hash, verifies
  // the canonical current slot before writing, reads the committed selection back,
  // and applies the live verifier only after durable verification. Any ambiguous
  // storage or runtime result faults this boot until restart.
  ConfigAdminPinUpdateResult commitAdminPinCredential(
      const uint8_t salt[8], const uint8_t hash[32],
      IConfigApplyRuntime& runtime);

  ConfigBootState state() const { return state_; }
  bool ready() const { return state_ == CFG_BOOT_READY; }
  bool needsSetup() const { return state_ == CFG_BOOT_NEEDS_SETUP; }
  bool faulted() const { return state_ == CFG_BOOT_FAULT; }
  const Config* config() const { return ready() ? &config_ : nullptr; }
  bool selection(CfgPick& out) const;

 private:
  ConfigBootResult result_(ConfigBootCode code,
                           ConfigProvisionCode provisionCode = CFG_PROVISION_NOT_UNPROVISIONED) const;
  void latchFault_();

  IConfigBootBackend& backend_;
  ConfigBootState state_;
  Config config_;
  CfgPick selected_;
};

}  // namespace att
