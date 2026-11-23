#include "esp32_nvs_closure_checkpoint_store.h"
#include "esp32_nvs_startup_guard.h"
#include <string.h>

namespace att {
namespace {
const char kNamespace[] = "attclos";
const char kKey[] = "tick_v1";
const char kInitializedKey[] = "ready_v1";
const uint16_t kInitializedValue = 0xC17C;

class WipeBytes {
 public:
  WipeBytes(uint8_t* bytes, size_t length) : bytes_(bytes), length_(length) {}
  WipeBytes(const WipeBytes&) = delete;
  WipeBytes& operator=(const WipeBytes&) = delete;
  ~WipeBytes() {
    volatile uint8_t* p = bytes_;
    for (size_t i = 0; i < length_; ++i) p[i] = 0;
  }
 private:
  uint8_t* bytes_;
  size_t length_;
};

bool validNow(const Config& config, uint32_t epoch) {
  if (epoch < EPOCH_2020) return false;
  const int32_t day = dayNumber(epoch, config.params.utcOffsetMin);
  return day >= config.goLiveDay && day <= MAX_SUPPORTED_DAY;
}
}  // namespace

Esp32NvsClosureCheckpointStore::Esp32NvsClosureCheckpointStore(
    const ConfigBootCoordinator& configBoot)
    : configBoot_(configBoot), faulted_(false), lastError_(ESP_OK) {}

void Esp32NvsClosureCheckpointStore::latch_(esp_err_t error) {
  faulted_ = true;
  lastError_ = error == ESP_OK ? ESP_FAIL : error;
}

bool Esp32NvsClosureCheckpointStore::ensureReady_() {
  if (faulted_) return false;
  const Esp32NvsStartupStatus startup = Esp32NvsStartupGuard::status();
  const Config* config = configBoot_.config();
  if (!startup.safeForApplication || !configBoot_.ready() || config == nullptr ||
      !validateConfig(*config)) {
    lastError_ = startup.safeForApplication ? ESP_ERR_NVS_NOT_INITIALIZED :
        (startup.firstInitResult == ESP_OK ? ESP_FAIL :
         static_cast<esp_err_t>(startup.firstInitResult));
    return false;
  }
  lastError_ = ESP_OK;
  return true;
}

ClosureCheckpointStoreCode Esp32NvsClosureCheckpointStore::read_(
    ClosureCheckpoint& out) {
  if (!ensureReady_()) return CCS_NOT_READY;

  nvs_handle_t handle;
  esp_err_t error = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return CCS_NOT_FOUND;
  if (error != ESP_OK) {
    latch_(error);
    return CCS_IO_ERROR;
  }

  // A separate create-only marker prevents a deleted/missing tick key from
  // being mistaken for first initialization later on the go-live day.
  uint16_t initialized = 0;
  error = nvs_get_u16(handle, kInitializedKey, &initialized);
  if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    latch_(error);
    return CCS_IO_ERROR;
  }
  const bool markerPresent = error == ESP_OK;
  if (markerPresent && initialized != kInitializedValue) {
    nvs_close(handle);
    latch_(ESP_FAIL);
    return CCS_INVALID;
  }

  size_t length = 0;
  error = nvs_get_blob(handle, kKey, nullptr, &length);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    if (markerPresent) {
      latch_(ESP_FAIL);  // Initialization marker with missing checkpoint.
      return CCS_INVALID;
    }
    return CCS_NOT_FOUND;  // Both create-only records are absent.
  }
  if (error != ESP_OK) {
    nvs_close(handle);
    latch_(error);
    return CCS_IO_ERROR;
  }
  if (!markerPresent) {
    nvs_close(handle);
    latch_(ESP_FAIL);
    return CCS_INVALID;
  }
  if (length != CLOSURE_CHECKPOINT_SIZE) {
    nvs_close(handle);
    latch_(ESP_ERR_NVS_INVALID_LENGTH);
    return CCS_INVALID;
  }

  uint8_t bytes[CLOSURE_CHECKPOINT_SIZE] = {};
  WipeBytes wipe(bytes, sizeof bytes);
  size_t actual = sizeof bytes;
  error = nvs_get_blob(handle, kKey, bytes, &actual);
  nvs_close(handle);
  if (error != ESP_OK) {
    latch_(error);
    return CCS_IO_ERROR;
  }
  if (actual != sizeof bytes) {
    latch_(ESP_ERR_NVS_INVALID_LENGTH);
    return CCS_INVALID;
  }

  const Config* config = configBoot_.config();
  ClosureCheckpoint decoded = {};
  const ClosureCheckpointDecodeCode code = decodeClosureCheckpoint(
      bytes, sizeof bytes, config->goLiveDay, config->params.utcOffsetMin,
      decoded);
  if (code != CCP_OK) {
    latch_(ESP_FAIL);
    return CCS_INVALID;
  }
  out = decoded;
  lastError_ = ESP_OK;
  return CCS_OK;
}

ClosureCheckpointStoreCode Esp32NvsClosureCheckpointStore::load(
    ClosureCheckpoint& out) {
  ClosureCheckpoint candidate = {};
  const ClosureCheckpointStoreCode code = read_(candidate);
  if (code == CCS_OK) out = candidate;
  return code;
}

ClosureCheckpointStoreCode Esp32NvsClosureCheckpointStore::writeAndVerify_(
    uint32_t nowEpoch, bool createOnly) {
  if (!ensureReady_()) return CCS_NOT_READY;
  const Config* config = configBoot_.config();
  if (!validNow(*config, nowEpoch)) {
    lastError_ = ESP_ERR_INVALID_ARG;
    return CCS_INVALID;
  }

  ClosureCheckpoint previous = {};
  const ClosureCheckpointStoreCode readCode = read_(previous);
  if (createOnly) {
    if (readCode == CCS_OK) return CCS_ALREADY_EXISTS;
    if (readCode != CCS_NOT_FOUND) return readCode;
    // A missing checkpoint AND marker is accepted only on the configured
    // go-live day. We refuse to synthesize an outage-free history later.
    if (dayNumber(nowEpoch, config->params.utcOffsetMin) != config->goLiveDay) {
      lastError_ = ESP_ERR_INVALID_ARG;
      return CCS_INVALID;
    }
  } else {
    if (readCode != CCS_OK) return readCode;
    if (nowEpoch < previous.lastTickEpoch) {
      latch_(ESP_FAIL);
      return CCS_TIME_ROLLBACK;
    }
    if (nowEpoch == previous.lastTickEpoch) return CCS_OK;
  }

  uint8_t bytes[CLOSURE_CHECKPOINT_SIZE] = {};
  WipeBytes wipe(bytes, sizeof bytes);
  if (!encodeClosureCheckpoint(config->goLiveDay, nowEpoch,
                               config->params.utcOffsetMin, bytes))
    return CCS_INVALID;

  nvs_handle_t handle;
  esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    latch_(error);
    return CCS_IO_ERROR;
  }

  // Recheck create-only while holding the serialized NVS owner. NVS has no
  // compare-and-swap; application-loop serialization is part of this contract.
  if (createOnly) {
    uint16_t initialized = 0;
    error = nvs_get_u16(handle, kInitializedKey, &initialized);
    if (error == ESP_OK) {
      nvs_close(handle);
      if (initialized == kInitializedValue) return CCS_ALREADY_EXISTS;
      latch_(ESP_FAIL);
      return CCS_INVALID;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      latch_(error);
      return CCS_IO_ERROR;
    }
    size_t existingLength = 0;
    error = nvs_get_blob(handle, kKey, nullptr, &existingLength);
    if (error == ESP_OK) {
      nvs_close(handle);
      latch_(ESP_FAIL);  // Checkpoint without its create-only marker is corrupt.
      return CCS_INVALID;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      latch_(error);
      return CCS_IO_ERROR;
    }
  }

  error = nvs_set_blob(handle, kKey, bytes, sizeof bytes);
  if (error == ESP_OK && createOnly)
    error = nvs_set_u16(handle, kInitializedKey, kInitializedValue);
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) {
    latch_(error);  // Commit outcome may be indeterminate.
    return CCS_IO_ERROR;
  }

  ClosureCheckpoint persisted = {};
  const ClosureCheckpointStoreCode verify = read_(persisted);
  if (verify != CCS_OK || persisted.goLiveDay != config->goLiveDay ||
      persisted.lastTickEpoch != nowEpoch) {
    if (!faulted_) latch_(ESP_FAIL);
    return verify == CCS_NOT_READY ? verify : CCS_INVALID;
  }
  lastError_ = ESP_OK;
  return CCS_OK;
}

ClosureCheckpointStoreCode Esp32NvsClosureCheckpointStore::initialize(
    uint32_t nowEpoch) {
  return writeAndVerify_(nowEpoch, true);
}

ClosureCheckpointStoreCode Esp32NvsClosureCheckpointStore::advance(
    uint32_t nowEpoch) {
  return writeAndVerify_(nowEpoch, false);
}

}  // namespace att
