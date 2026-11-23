#if defined(ESP_PLATFORM)
#include "esp32_recovery_service.h"

namespace att {

Esp32RecoveryService::Esp32RecoveryService(
    ILittleFsStorageOwner& normalStorageOwner,
    const char* mountpoint, const char* partitionLabel,
    int16_t utcOffsetMin, ISecureRandomSource& random)
    : normalStorageOwner_(normalStorageOwner),
      mountBackend_(true),
      storageOwner_(mountBackend_, mountpoint, partitionLabel, utcOffsetMin),
      sourceFiles_(mountpoint, false),
      sourceMonths_(sourceFiles_),
      recoveryBackend_(mountpoint, partitionLabel, true),
      journal_(recoveryBackend_),
      previewGate_(),
      random_(random),
      presence_(nullptr),
      utcOffsetMin_(utcOffsetMin),
      active_(false),
      cleanupOnly_(false),
      normalOwnerCleanupPending_(false),
      restartRequired_(false) {}

Esp32RecoveryService::~Esp32RecoveryService() {
  if (active_ || normalOwnerCleanupPending_) (void)end();
}

bool Esp32RecoveryService::begin(IRecoveryPhysicalPresence& presence) {
  if (active_) {
    if (cleanupOnly_) {
      (void)end();
      return false;
    }
    if (presence_ != &presence) return false;
    if (presence.asserted()) return true;
    revokePresence_();
    return false;
  }
  // A previous handoff attempt may have unmounted normal storage or left a
  // recovery mount requiring cleanup. Never retry in the same app lifetime:
  // cached LogStore/Roster/Closure objects may no longer match disk state.
  if (restartRequired_) return false;
  previewGate_.clear();
  if (!presence.asserted() || !sourceFiles_.valid() ||
      !recoveryBackend_.valid()) return false;

  // Enforce exclusive ownership of the signed partition. READY is quiesced
  // through the exact normal owner; NOT_STARTED or a fully cleaned FAULT means
  // it has no owned mount. Any STARTING/cleanup-failed/recovery state blocks.
  const LittleFsOwnerState normalState = normalStorageOwner_.state();
  if (normalState == LITTLEFS_OWNER_READY) {
    if (normalStorageOwner_.writable() ||
        normalStorageOwner_.fileSet() == nullptr) return false;
    // end() closes the serialized file-set gate before attempting unmount.
    // Even a failed unmount invalidates the old projection graph, so latch the
    // mandatory reboot before making that transition attempt.
    restartRequired_ = true;
    if (!normalStorageOwner_.end()) {
      normalOwnerCleanupPending_ = true;
      return false;
    }
    normalOwnerCleanupPending_ = false;
  } else if (normalState == LITTLEFS_OWNER_NOT_STARTED ||
             (normalState == LITTLEFS_OWNER_FAULT &&
              !normalStorageOwner_.cleanupFailed())) {
    // From this point onward the current application must reboot, even if the
    // writable mount cannot be acquired. Do not revive stale in-memory owners.
    restartRequired_ = true;
  } else {
    return false;
  }
  if (!presence.asserted() || !storageOwner_.beginRecovery())
    return false;

  // Mount registration can take time. Re-sample the physical control after
  // the writable mount is established; if it was released meanwhile, do not
  // expose an operational service and immediately attempt owned unmount.
  presence_ = &presence;
  active_ = true;
  if (!presence.asserted()) {
    cleanupOnly_ = true;
    (void)end();
    return false;
  }
  return true;
}

RecoveryResult Esp32RecoveryService::inactiveResult_() const {
  RecoveryResult result = {};
  result.status = RECOVERY_READ_ONLY;
  return result;
}

void Esp32RecoveryService::revokePresence_() {
  if (!active_) return;
  cleanupOnly_ = true;
  (void)end();
}

RecoveryPreview Esp32RecoveryService::preview() {
  previewGate_.clear();
  RecoveryPreview preview = {};
  if (!active_ || cleanupOnly_) {
    preview.status = RECOVERY_READ_ONLY;
    return preview;
  }
  if (presence_ == nullptr || !presence_->asserted()) {
    preview.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return preview;
  }
  uint8_t operationId[RECOVERY_OPERATION_ID_BYTES];
  if (!generateRecoveryOperationId(random_, operationId)) {
    preview.status = RECOVERY_BACKEND_FAILURE;
    return preview;
  }
  preview = journal_.preview(operationId, sourceMonths_, utcOffsetMin_);
  if (presence_ == nullptr || !presence_->asserted()) {
    previewGate_.clear();
    preview.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return preview;
  }
  if (preview.status == RECOVERY_PREVIEW_READY)
    previewGate_.issueInitial(preview);
  return preview;
}

RecoveryResult Esp32RecoveryService::prepare(
    const RecoveryPreview& preview,
    AdminRecoveryAuthorization& authorization) {
  if (!active_ || cleanupOnly_) return inactiveResult_();
  if (!previewGate_.consumeInitial(preview)) {
    RecoveryResult result = {};
    result.status = RECOVERY_STALE_PREVIEW;
    return result;
  }
  if (presence_ == nullptr || !presence_->asserted()) {
    RecoveryResult result = {};
    result.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return result;
  }
  if (!authorization.usesPresence(*presence_)) {
    RecoveryResult result = {};
    result.status = RECOVERY_AUTH_REQUIRED;
    return result;
  }
  RecoveryResult result =
      journal_.prepare(preview, sourceMonths_, utcOffsetMin_, authorization);
  if (presence_ == nullptr || !presence_->asserted()) revokePresence_();
  return result;
}

RecoveryPreview Esp32RecoveryService::pendingPreview() {
  previewGate_.clear();
  RecoveryPreview preview = {};
  if (!active_ || cleanupOnly_) {
    preview.status = RECOVERY_READ_ONLY;
    return preview;
  }
  if (presence_ == nullptr || !presence_->asserted()) {
    preview.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return preview;
  }
  preview = journal_.pendingPreview();
  if (presence_ == nullptr || !presence_->asserted()) {
    previewGate_.clear();
    preview.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return preview;
  }
  if (preview.status == RECOVERY_PREVIEW_READY)
    previewGate_.issuePending(preview);
  return preview;
}

RecoveryResult Esp32RecoveryService::resumePending(
    const RecoveryPreview& preview,
    AdminRecoveryAuthorization& authorization) {
  if (!active_ || cleanupOnly_) return inactiveResult_();
  if (!previewGate_.consumePending(preview)) {
    RecoveryResult result = {};
    result.status = RECOVERY_STALE_PREVIEW;
    return result;
  }
  if (presence_ == nullptr || !presence_->asserted()) {
    RecoveryResult result = {};
    result.status = RECOVERY_PRESENCE_REQUIRED;
    revokePresence_();
    return result;
  }
  if (!authorization.usesPresence(*presence_)) {
    RecoveryResult result = {};
    result.status = RECOVERY_AUTH_REQUIRED;
    return result;
  }
  RecoveryResult result = journal_.resumePending(preview, authorization);
  if (presence_ == nullptr || !presence_->asserted()) revokePresence_();
  return result;
}

bool Esp32RecoveryService::end() {
  if (normalOwnerCleanupPending_) {
    // The normal owner may have closed its serialized gate but failed to
    // unregister. Its end() contract explicitly supports cleanup retry.
    if (!normalStorageOwner_.end()) return false;
    normalOwnerCleanupPending_ = false;
    return true;
  }
  if (!active_) return true;
  previewGate_.clear();
  // Once closure is requested, the service becomes non-operational even if
  // unregister fails. Only end() may be retried in cleanup-only state.
  cleanupOnly_ = true;
  if (!storageOwner_.endRecovery()) return false;
  active_ = false;
  cleanupOnly_ = false;
  presence_ = nullptr;
  return true;
}

}  // namespace att
#endif  // ESP_PLATFORM
