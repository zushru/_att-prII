#include "littlefs_serialized_fileset.h"

namespace att {
namespace {

class LockGuard {
 public:
  explicit LockGuard(ILittleFsStorageLock& lock)
      : lock_(lock), acquired_(lock_.acquire()) {}
  ~LockGuard() {
    if (acquired_) lock_.release();
  }
  bool acquired() const { return acquired_; }

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  ILittleFsStorageLock& lock_;
  bool acquired_;
};

}  // namespace

LittleFsStorageMutex::LittleFsStorageMutex()
#if defined(ESP_PLATFORM)
    : mutex_(xSemaphoreCreateMutex())
#endif
{}

LittleFsStorageMutex::~LittleFsStorageMutex() {
#if defined(ESP_PLATFORM)
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
#endif
}

bool LittleFsStorageMutex::valid() const {
#if defined(ESP_PLATFORM)
  return mutex_ != nullptr;
#else
  return true;
#endif
}

bool LittleFsStorageMutex::acquire() {
#if defined(ESP_PLATFORM)
  return mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
#else
  mutex_.lock();
  return true;
#endif
}

void LittleFsStorageMutex::release() {
#if defined(ESP_PLATFORM)
  if (mutex_) (void)xSemaphoreGive(mutex_);
#else
  mutex_.unlock();
#endif
}

SerializedFileSet::SerializedFileSet(
    IFileSet& files, ILittleFsStorageLock& lock, bool writable)
    : files_(files), lock_(lock), writable_(writable), enabled_(true) {}

bool SerializedFileSet::countChecked(size_t& out) {
  out = 0;
  if (!enabled()) return false;
  LockGuard guard(lock_);
  if (!guard.acquired() || !enabled() || !files_.countChecked(out)) {
    out = 0;
    return false;
  }
  return true;
}

bool SerializedFileSet::monthKeyChecked(size_t index, uint32_t& out) {
  out = 0;
  if (!enabled()) return false;
  LockGuard guard(lock_);
  if (!guard.acquired() || !enabled() ||
      !files_.monthKeyChecked(index, out)) {
    out = 0;
    return false;
  }
  return true;
}

bool SerializedFileSet::fileSizeChecked(size_t index, size_t& out) {
  out = 0;
  if (!enabled()) return false;
  LockGuard guard(lock_);
  if (!guard.acquired() || !enabled() ||
      !files_.fileSizeChecked(index, out)) {
    out = 0;
    return false;
  }
  return true;
}

bool SerializedFileSet::readFile(size_t index, size_t offset, uint8_t* out,
                                 size_t count) {
  if (!enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() &&
      files_.readFile(index, offset, out, count);
}

bool SerializedFileSet::appendFile(size_t index, const uint8_t* data,
                                   size_t count) {
  if (!writable() || !enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() && writable() &&
      files_.appendFile(index, data, count);
}

bool SerializedFileSet::truncateFile(size_t index, size_t length) {
  if (!writable() || !enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() && writable() &&
      files_.truncateFile(index, length);
}

bool SerializedFileSet::addFile(uint32_t monthKey) {
  if (!writable() || !enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() && writable() && files_.addFile(monthKey);
}

bool SerializedFileSet::quarantineCopy(size_t index) {
  if (!writable() || !enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() && writable() && files_.quarantineCopy(index);
}

bool SerializedFileSet::quarantineFrom(size_t index) {
  if (!writable() || !enabled()) return false;
  LockGuard guard(lock_);
  return guard.acquired() && enabled() && writable() && files_.quarantineFrom(index);
}

}  // namespace att
