#include "esp32_nvs_restore_journal_store.h"
#include "esp32_nvs_startup_guard.h"
#include <string.h>

namespace att {
namespace {

const char* const JOURNAL_NAMESPACE = "attrstx";
const char* const JOURNAL_KEY = "journal";

void secureClear(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *bytes++ = 0;
}

bool sameJournal(const RestoreTxJournal& left,
                 const RestoreTxJournal& right) {
  uint8_t a[RESTORE_TX_JOURNAL_BYTES] = {};
  uint8_t b[RESTORE_TX_JOURNAL_BYTES] = {};
  const bool encoded = encodeRestoreTxJournal(left, a) &&
      encodeRestoreTxJournal(right, b);
  const bool same = encoded && memcmp(a, b, sizeof a) == 0;
  secureClear(a, sizeof a);
  secureClear(b, sizeof b);
  return same;
}

bool sameTransactionExceptPhase(const RestoreTxJournal& current,
                                const RestoreTxJournal& next) {
  RestoreTxJournal expected = current;
  expected.phase = next.phase;
  expected.journalCrc32 = calculateRestoreTxJournalCrc32(expected);
  const bool same = sameJournal(expected, next);
  secureClear(&expected, sizeof expected);
  return same;
}

}  // namespace

bool Esp32NvsRestoreJournalStore::ensureReady_() const {
  if (faulted_) return false;
  if (!ready_) {
    if (lastError_ == ESP_OK) lastError_ = ESP_ERR_NVS_NOT_INITIALIZED;
    return false;
  }
  const Esp32NvsStartupStatus startup = Esp32NvsStartupGuard::status();
  if (!startup.safeForApplication) {
    ready_ = false;
    faulted_ = true;
    lastError_ = startup.firstInitResult == ESP_OK
        ? ESP_FAIL : static_cast<esp_err_t>(startup.firstInitResult);
    return false;
  }
  return true;
}

bool Esp32NvsRestoreJournalStore::ready() const {
  return ensureReady_();
}

bool Esp32NvsRestoreJournalStore::begin() {
  if (faulted_) return false;
  if (ready_) return ensureReady_();
  const Esp32NvsStartupStatus startup = Esp32NvsStartupGuard::status();
  if (!startup.safeForApplication) {
    lastError_ = startup.firstInitResult == ESP_OK
        ? ESP_FAIL : static_cast<esp_err_t>(startup.firstInitResult);
    ready_ = false;
    faulted_ = true;
    return false;
  }
  ready_ = true;
  lastError_ = ESP_OK;
  return true;
}

bool Esp32NvsRestoreJournalStore::readJournal(
    RestoreTxJournal& out, bool& present, bool& ambiguous) {
  secureClear(&out, sizeof out);
  present = false;
  ambiguous = false;
  if (!ensureReady_()) return false;

  nvs_handle_t handle;
  esp_err_t error = nvs_open(JOURNAL_NAMESPACE, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    lastError_ = ESP_OK;
    return true;
  }
  if (error != ESP_OK) {
    lastError_ = error;
    return false;
  }

  size_t length = 0;
  error = nvs_get_blob(handle, JOURNAL_KEY, nullptr, &length);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    lastError_ = ESP_OK;
    return true;
  }
  if (error != ESP_OK) {
    nvs_close(handle);
    lastError_ = error;
    return false;
  }
  present = true;
  if (length != RESTORE_TX_JOURNAL_BYTES) {
    // Unknown/truncated journal bytes must never be mistaken for no pending
    // restore. Leave them untouched for service inspection.
    ambiguous = true;
    nvs_close(handle);
    lastError_ = ESP_ERR_NVS_INVALID_LENGTH;
    return true;
  }

  uint8_t encoded[RESTORE_TX_JOURNAL_BYTES] = {};
  size_t actual = sizeof encoded;
  error = nvs_get_blob(handle, JOURNAL_KEY, encoded, &actual);
  nvs_close(handle);
  if (error != ESP_OK) {
    secureClear(encoded, sizeof encoded);
    lastError_ = error;
    return false;
  }
  if (actual != sizeof encoded) {
    ambiguous = true;
    secureClear(encoded, sizeof encoded);
    lastError_ = ESP_ERR_NVS_INVALID_LENGTH;
    return true;
  }

  // A correctly-sized but invalid record is returned as present and corrupt;
  // the coordinator then latches service-only recovery instead of overwriting.
  if (!decodeRestoreTxJournal(encoded, sizeof encoded, out)) {
    secureClear(encoded, sizeof encoded);
    lastError_ = ESP_OK;
    return true;
  }
  secureClear(encoded, sizeof encoded);
  lastError_ = ESP_OK;
  return true;
}

bool Esp32NvsRestoreJournalStore::writeJournal(
    const RestoreTxJournal& journal) {
  if (!ensureReady_() || !validateRestoreTxJournal(journal)) {
    if (lastError_ == ESP_OK) lastError_ = ESP_ERR_INVALID_ARG;
    return false;
  }

  RestoreTxJournal current = {};
  bool present = false;
  bool ambiguous = false;
  if (!readJournal(current, present, ambiguous)) {
    secureClear(&current, sizeof current);
    return false;
  }
  if (ambiguous || (present && !validateRestoreTxJournal(current))) {
    secureClear(&current, sizeof current);
    lastError_ = ESP_ERR_NVS_INVALID_STATE;
    return false;
  }
  if (!present) {
    if (journal.phase != RESTORE_TX_PHASE_PREPARED) {
      secureClear(&current, sizeof current);
      lastError_ = ESP_ERR_INVALID_STATE;
      return false;
    }
  } else {
    const bool sameOperation =
        memcmp(current.operationId, journal.operationId,
               sizeof current.operationId) == 0;
    const bool phaseStep = journal.phase == current.phase + 1u;
    const bool exactRetry = sameJournal(current, journal);
    if (!sameOperation || (!phaseStep && !exactRetry) ||
        (phaseStep && !sameTransactionExceptPhase(current, journal))) {
      secureClear(&current, sizeof current);
      lastError_ = ESP_ERR_INVALID_STATE;
      return false;
    }
    if (exactRetry) {
      secureClear(&current, sizeof current);
      lastError_ = ESP_OK;
      return true;
    }
  }
  secureClear(&current, sizeof current);

  uint8_t encoded[RESTORE_TX_JOURNAL_BYTES] = {};
  if (!encodeRestoreTxJournal(journal, encoded)) {
    secureClear(encoded, sizeof encoded);
    lastError_ = ESP_ERR_INVALID_ARG;
    return false;
  }
  nvs_handle_t handle;
  esp_err_t error = nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &handle);
  if (error == ESP_OK) {
    error = nvs_set_blob(handle, JOURNAL_KEY, encoded, sizeof encoded);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
  }
  secureClear(encoded, sizeof encoded);
  lastError_ = error;
  return error == ESP_OK;
}

bool Esp32NvsRestoreJournalStore::clearJournal(
    const RestoreTxJournal& expected) {
  if (!ensureReady_() || !validateRestoreTxJournal(expected) ||
      expected.phase != RESTORE_TX_PHASE_ACKNOWLEDGED) {
    if (lastError_ == ESP_OK) lastError_ = ESP_ERR_INVALID_ARG;
    return false;
  }
  RestoreTxJournal current = {};
  bool present = false;
  bool ambiguous = false;
  if (!readJournal(current, present, ambiguous)) {
    secureClear(&current, sizeof current);
    return false;
  }
  if (!present) {
    secureClear(&current, sizeof current);
    lastError_ = ESP_OK;
    return true;  // Idempotent after a clear committed but its response was lost.
  }
  if (ambiguous || !sameJournal(current, expected)) {
    secureClear(&current, sizeof current);
    lastError_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  secureClear(&current, sizeof current);

  nvs_handle_t handle;
  esp_err_t error = nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &handle);
  if (error == ESP_OK) {
    error = nvs_erase_key(handle, JOURNAL_KEY);
    // A concurrent idempotent clear may remove the key after our readback.
    // Treat NOT_FOUND as an already-applied erase, then verify absence below.
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
  } else if (error == ESP_ERR_NVS_NOT_FOUND) {
    error = ESP_OK;
  }
  if (error == ESP_OK) {
    RestoreTxJournal check = {};
    bool stillPresent = false;
    bool stillAmbiguous = false;
    const bool verified = readJournal(check, stillPresent, stillAmbiguous) &&
        !stillPresent && !stillAmbiguous;
    secureClear(&check, sizeof check);
    if (!verified) error = ESP_FAIL;
  }
  lastError_ = error;
  return error == ESP_OK;
}

}  // namespace att
