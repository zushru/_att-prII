#pragma once
#include <stddef.h>
#include <stdint.h>
namespace att {
// One logical append-only byte stream of 16-byte records. Month files, quarantine and
// truncate-by-rewrite live in the driver, not here.
class IStorage {
 public:
  virtual ~IStorage() {}
  // Fail closed unless the concrete storage explicitly authorizes mutation.
  virtual bool writable() const { return false; }
  // True only when a successful append already includes an exact byte-for-byte
  // readback of the complete appended range after its durability operation.
  // Unknown and generic backends default to false, so higher layers retain their
  // independent post-commit readback unless the concrete driver proves this.
  virtual bool appendVerifiesExactBytes() const { return false; }
  // Every storage driver must distinguish an empty stream from a failed size query.
  virtual bool sizeChecked(size_t& out) = 0;
  // Convenience for non-critical display/debug callers. SIZE_MAX means failure;
  // boot/replay code must use sizeChecked() and inspect its boolean result.
  size_t size() { size_t out = 0; return sizeChecked(out) ? out : (size_t)-1; }
  virtual bool read(size_t off, uint8_t* buf, size_t n) = 0;
  // true = complete bytes passed the driver's durability criterion.
  // false = indeterminate: zero, partial, or complete bytes may be present.
  virtual bool append(const uint8_t* buf, size_t n) = 0;
  virtual bool truncate(size_t n) = 0;
};
}
