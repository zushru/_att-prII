#include "month_set.h"
#include "backup_format.h"
#include "event_codec.h"
#include "sha256.h"
#include <string.h>
namespace att {
bool MonthSet::sizeChecked(size_t& out) {
  out = 0;
  size_t count = 0;
  if (!f_.countChecked(count)) return false;
  for (size_t i = 0; i < count; ++i) {
    size_t bytes = 0;
    if (!f_.fileSizeChecked(i, bytes) ||
        (i + 1 < count && (bytes % REC_SIZE) != 0) ||
        bytes > (size_t)-1 - out)
      return false;
    out += bytes;
  }
  return true;
}
bool MonthSet::read(size_t off, uint8_t* buf, size_t n) {
  if (n == 0) return true;
  if (!buf) return false;
  size_t count = 0;
  if (!f_.countChecked(count)) return false;
  size_t start = 0;
  for (size_t i = 0; i < count && n > 0; ++i) {
    size_t bytes = 0;
    if (!f_.fileSizeChecked(i, bytes)) return false;
    if (off >= start && off - start < bytes) {
      const size_t local = off - start;
      size_t take = bytes - local;
      if (take > n) take = n;
      if (!f_.readFile(i, local, buf, take)) return false;
      buf += take;
      off += take;
      n -= take;
    }
    if (bytes > (size_t)-1 - start) return false;
    start += bytes;
  }
  return n == 0;
}
bool MonthSet::inspectInventory(MonthFileInventoryEntry* entries,
                                 size_t capacity, MonthSetInventory& out) {
  out = MonthSetInventory{0, 0, 0, {0}};
  size_t count = 0;
  if (!f_.countChecked(count) || count > capacity ||
      (count != 0 && entries == nullptr) || count > UINT32_MAX) return false;

  Crc32 manifest;
  Sha256 strongManifest;
  uint8_t manifestHeader[8] = {'M', 'S', 'I', 1, 0, 0, 0, 0};
  const uint32_t fileCount32 = (uint32_t)count;
  for (size_t b = 0; b < 4; ++b)
    manifestHeader[4 + b] = (uint8_t)(fileCount32 >> (8u * b));
  manifest.add(manifestHeader, sizeof manifestHeader);
  strongManifest.update(manifestHeader, sizeof manifestHeader);
  uint64_t totalBytes = 0;
  uint32_t previousKey = 0;
  uint8_t buffer[256];

  for (size_t i = 0; i < count; ++i) {
    uint32_t key = 0;
    size_t size = 0;
    if (!f_.monthKeyChecked(i, key)) return false;
    const uint32_t year = key / 100u;
    const uint32_t month = key % 100u;
    if (year == 0 || year > 9999 || month == 0 || month > 12 ||
        (i != 0 && key <= previousKey) ||
        !f_.fileSizeChecked(i, size) ||
        (i + 1 < count && (size % REC_SIZE) != 0) ||
        (uint64_t)size > UINT64_MAX - totalBytes) return false;

    Crc32 content;
    Sha256 strongContent;
    size_t offset = 0;
    while (offset < size) {
      const size_t chunk = size - offset > sizeof buffer
          ? sizeof buffer : size - offset;
      if (!f_.readFile(i, offset, buffer, chunk)) return false;
      content.add(buffer, chunk);
      strongContent.update(buffer, chunk);
      offset += chunk;
    }

    // Recheck the identity and length after reading. This catches metadata
    // changes during inspection; callers still must quiesce all writers.
    uint32_t keyAfter = 0;
    size_t sizeAfter = 0;
    if (!f_.monthKeyChecked(i, keyAfter) || keyAfter != key ||
        !f_.fileSizeChecked(i, sizeAfter) || sizeAfter != size) return false;

    entries[i] = MonthFileInventoryEntry{
        key, (uint64_t)size, content.value(), {0}};
    strongContent.finish(entries[i].contentSha256);
    uint8_t encoded[16];
    for (size_t b = 0; b < 4; ++b)
      encoded[b] = (uint8_t)(key >> (8u * b));
    const uint64_t wideSize = (uint64_t)size;
    for (size_t b = 0; b < 8; ++b)
      encoded[4 + b] = (uint8_t)(wideSize >> (8u * b));
    const uint32_t fileCrc = content.value();
    for (size_t b = 0; b < 4; ++b)
      encoded[12 + b] = (uint8_t)(fileCrc >> (8u * b));
    manifest.add(encoded, sizeof encoded);
    manifest.add(entries[i].contentSha256,
                 sizeof entries[i].contentSha256);
    strongManifest.update(encoded, sizeof encoded);
    strongManifest.update(entries[i].contentSha256,
                          sizeof entries[i].contentSha256);

    totalBytes += (uint64_t)size;
    previousKey = key;
  }

  // Recheck the complete ordered metadata snapshot after the content pass.
  // This catches additions/removals and size/key changes between file reads.
  size_t finalCount = 0;
  if (!f_.countChecked(finalCount) || finalCount != count) return false;
  for (size_t i = 0; i < count; ++i) {
    uint32_t key = 0;
    size_t size = 0;
    if (!f_.monthKeyChecked(i, key) || key != entries[i].monthKey ||
        !f_.fileSizeChecked(i, size) ||
        (uint64_t)size != entries[i].byteSize) return false;
  }

  out.fileCount = count;
  out.totalBytes = totalBytes;
  out.manifestCrc32 = manifest.value();
  strongManifest.finish(out.manifestSha256);
  return true;
}

bool MonthSet::append(const uint8_t* buf, size_t n) {
  if (!f_.writable() || month_ == 0 || (!buf && n) || n == 0) return false;
  size_t count = 0;
  if (!f_.countChecked(count)) return false;
  bool create = count == 0;
  if (count > 0) {
    uint32_t newestKey = 0;
    size_t newestSize = 0;
    if (!f_.monthKeyChecked(count - 1, newestKey) ||
        !f_.fileSizeChecked(count - 1, newestSize)) return false;
    create = month_ > newestKey;
    if (create && (newestSize % 16) != 0) return false;
  }
  if (create) {
    if (count == (size_t)-1 || !f_.addFile(month_)) return false;
    size_t after = 0, newSize = 0;
    uint32_t newKey = 0;
    if (!f_.countChecked(after) || after != count + 1 || after == 0 ||
        !f_.monthKeyChecked(after - 1, newKey) || newKey != month_ ||
        !f_.fileSizeChecked(after - 1, newSize) || newSize != 0)
      return false;
    count = after;
  }
  return count > 0 && f_.appendFile(count - 1, buf, n);
}
bool MonthSet::truncate(size_t) {
  // Do not let a writable IFileSet turn this into an unjournaled mutation.
  return false;
}
QuarantinePlan MonthSet::planAt(size_t off) {
  QuarantinePlan p = {false, 0, 0};
  size_t count = 0;
  if (!f_.countChecked(count)) return p;
  size_t start = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t bytes = 0;
    if (!f_.fileSizeChecked(i, bytes) || bytes > (size_t)-1 - start)
      return p;
    if (off >= start && off - start < bytes) {
      p.valid = true;
      p.fileIndex = i;
      p.localOff = off - start;
      return p;
    }
    start += bytes;
  }
  return p;
}
bool MonthSet::repair(const QuarantinePlan&) {
  // The former copy/move/truncate sequence had no restart-safe commit point.
  // Keep the plan useful for diagnostics, but never execute it here.
  return false;
}
}
