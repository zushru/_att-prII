#include "config_provision.h"

namespace att {
namespace {

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

ConfigProvisionResult provisionResult(ConfigProvisionCode code,
                                      CfgPick selected = {CB_FAULT, -1, 0}) {
  return ConfigProvisionResult{code, selected};
}
}

ConfigProvisionResult ConfigProvisioner::provision(const Config& initial,
                                                     const CfgPick& loaded) {
  if (faulted_) return provisionResult(CFG_PROVISION_FAULTED);
  if (!gate_.mayProvision()) return provisionResult(CFG_PROVISION_NOT_AUTHORIZED);
  if (loaded.boot != CB_UNPROVISIONED || loaded.active != -1 || loaded.seq != 0)
    return provisionResult(CFG_PROVISION_NOT_UNPROVISIONED);
  if (!validateConfig(initial)) return provisionResult(CFG_PROVISION_INVALID_CONFIG);

  // Independently re-read the durable marker. A stale caller-supplied
  // CB_UNPROVISIONED result must not permit reprovisioning after both slot keys
  // have been deleted while the marker survives.
  bool markerPresent = false;
  if (!backend_.readProvisionMarker(markerPresent))
    return provisionResult(CFG_PROVISION_READ_FAILED);
  if (markerPresent)
    return provisionResult(CFG_PROVISION_NOT_UNPROVISIONED);

  // Re-read storage immediately before writing. The caller's earlier boot result
  // is not sufficient evidence that both slots remain blank.
  uint8_t a[CFG_SLOT_SIZE] = {}, b[CFG_SLOT_SIZE] = {};
  ScopedWipe wipeA(a, sizeof a);
  ScopedWipe wipeB(b, sizeof b);
  if (!backend_.readSlot(0, a) || !backend_.readSlot(1, b))
    return provisionResult(CFG_PROVISION_READ_FAILED);
  Config current;
  ScopedWipe wipeCurrent(&current, sizeof current);
  const CfgPick now = pickSlot(a, b, current);
  if (now.boot != CB_UNPROVISIONED || now.active != -1 || now.seq != 0)
    return provisionResult(CFG_PROVISION_NOT_UNPROVISIONED);

  uint8_t encoded[CFG_SLOT_SIZE] = {};
  ScopedWipe wipeEncoded(encoded, sizeof encoded);
  if (encodeConfig(initial, 1, encoded) == 0)
    return provisionResult(CFG_PROVISION_INVALID_CONFIG);

  // A false write may still have persisted bytes. Never retry in this boot.
  if (!backend_.writeSlot(0, encoded)) {
    faulted_ = true;
    return provisionResult(CFG_PROVISION_STORAGE_INDETERMINATE);
  }

  uint8_t verifyA[CFG_SLOT_SIZE] = {}, verifyB[CFG_SLOT_SIZE] = {};
  ScopedWipe wipeVerifyA(verifyA, sizeof verifyA);
  ScopedWipe wipeVerifyB(verifyB, sizeof verifyB);
  if (!backend_.readSlot(0, verifyA) || !backend_.readSlot(1, verifyB)) {
    faulted_ = true;
    return provisionResult(CFG_PROVISION_VERIFY_FAILED);
  }
  Config verified, unused;
  ScopedWipe wipeVerified(&verified, sizeof verified);
  ScopedWipe wipeUnused(&unused, sizeof unused);
  uint32_t unusedSeq = 0;
  const CfgPick selected = pickSlot(verifyA, verifyB, verified);
  if (selected.boot != CB_OK || selected.active != 0 || selected.seq != 1 ||
      !configEqual(verified, initial) || decodeConfig(verifyB, unused, unusedSeq) != CD_BLANK) {
    faulted_ = true;
    return provisionResult(CFG_PROVISION_VERIFY_FAILED);
  }
  return provisionResult(CFG_PROVISION_OK, selected);
}

}  // namespace att
