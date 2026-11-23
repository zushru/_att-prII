#pragma once

#include "restore_transaction.h"
#include <nvs.h>

namespace att {

// Durable NVS journal store for the restore transaction coordinator. The
// journal is encoded field-by-field in a fixed little-endian wire format; the
// in-memory C++ object is never written as a raw blob. NVS encryption is not
// assumed. This component does not implement filesystem promotion.
class Esp32NvsRestoreJournalStore final : public IRestoreTransactionJournalStore {
 public:
  Esp32NvsRestoreJournalStore()
      : ready_(false), faulted_(false), lastError_(ESP_OK) {}

  // Reuses Arduino-ESP32's already-completed guarded NVS initialization. It
  // never initializes, erases or reformats the NVS partition itself.
  bool begin();
  bool ready() const;
  esp_err_t lastError() const { return lastError_; }

  bool readJournal(RestoreTxJournal& out, bool& present,
                   bool& ambiguous) override;
  bool writeJournal(const RestoreTxJournal& journal) override;
  bool clearJournal(const RestoreTxJournal& expected) override;

 private:
  bool ensureReady_() const;
  mutable bool ready_;
  mutable bool faulted_;
  mutable esp_err_t lastError_;
};

}  // namespace att
