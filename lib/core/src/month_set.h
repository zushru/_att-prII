#pragma once
#include "istorage.h"
#include "fileset.h"
#include <stdint.h>
namespace att {
struct QuarantinePlan { bool valid; size_t fileIndex; size_t localOff; };

// Read-only source fingerprint for a recovery plan. CRC32 detects accidental
// changes; it is not an authenticity or collision-resistance guarantee.
struct MonthFileInventoryEntry {
  uint32_t monthKey;
  uint64_t byteSize;
  uint32_t contentCrc32;
  uint8_t contentSha256[32];
};

struct MonthSetInventory {
  size_t fileCount;
  uint64_t totalBytes;
  uint32_t manifestCrc32;
  uint8_t manifestSha256[32];
};

// One logical append-only stream over month files. Commit groups never straddle files.
class MonthSet : public IStorage {
 public:
  explicit MonthSet(IFileSet& f) : f_(f), month_(0) {}
  bool writable() const override { return f_.writable(); }
  bool appendVerifiesExactBytes() const override {
    return f_.appendVerifiesExactBytes();
  }
  void setMonth(uint32_t key) { month_ = key; }     // caller sets from the RTC before appending
  bool usesFileSet(const IFileSet& files) const { return &f_ == &files; }
  bool sizeChecked(size_t& out) override;
  bool read(size_t off, uint8_t* buf, size_t n) override;
  // Captures a non-mutating, ordered month-file inventory. Every non-final
  // month boundary must be REC_SIZE-aligned; the final file may contain a torn
  // tail. The caller must quiesce writers for the duration; on false, out is
  // zero and entries are unusable (some earlier slots may have been filled).
  bool inspectInventory(MonthFileInventoryEntry* entries, size_t capacity,
                        MonthSetInventory& out);
  bool append(const uint8_t* buf, size_t n) override;
  // Destructive operations remain hard-disabled here, even when a driver is
  // writable. They require an accepted crash-safe recovery transaction first.
  bool truncate(size_t n) override;
  QuarantinePlan planAt(size_t byteOff);            // read-only offset-to-file diagnostic
  bool repair(const QuarantinePlan& p);              // always refuses; never mutates
 private:
  IFileSet& f_;
  uint32_t month_;
};
}
