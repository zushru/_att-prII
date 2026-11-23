#pragma once

#include "fileset.h"
#include <atomic>

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <mutex>
#endif

namespace att {

class LittleFsStorageOwner;

class ILittleFsStorageLock {
 public:
  virtual ~ILittleFsStorageLock() {}
  virtual bool acquire() = 0;
  virtual void release() = 0;
};

// Task-context mutex used by the production storage boundary.
class LittleFsStorageMutex final : public ILittleFsStorageLock {
 public:
  LittleFsStorageMutex();
  ~LittleFsStorageMutex() override;

  LittleFsStorageMutex(const LittleFsStorageMutex&) = delete;
  LittleFsStorageMutex& operator=(const LittleFsStorageMutex&) = delete;

  bool valid() const;
  bool acquire() override;
  void release() override;

 private:
#if defined(ESP_PLATFORM)
  SemaphoreHandle_t mutex_;
#else
  std::mutex mutex_;
#endif
};

// Decorates every IFileSet call with one shared lock. The underlying file set
// must not be exposed through another route while this wrapper is in use.
class SerializedFileSet final : public IFileSet {
 public:
  SerializedFileSet(IFileSet& files, ILittleFsStorageLock& lock,
                    bool writable = false);
  bool writable() const override { return writable_.load(); }
  bool appendVerifiesExactBytes() const override {
    return enabled() && files_.appendVerifiesExactBytes();
  }
  bool enabled() const { return enabled_.load(); }

  SerializedFileSet(const SerializedFileSet&) = delete;
  SerializedFileSet& operator=(const SerializedFileSet&) = delete;

  bool countChecked(size_t& out) override;
  bool monthKeyChecked(size_t index, uint32_t& out) override;
  bool fileSizeChecked(size_t index, size_t& out) override;
  bool readFile(size_t index, size_t offset, uint8_t* out,
                size_t count) override;
  bool appendFile(size_t index, const uint8_t* data, size_t count) override;
  bool truncateFile(size_t index, size_t length) override;
  bool addFile(uint32_t monthKey) override;
  bool quarantineCopy(size_t index) override;
  bool quarantineFrom(size_t index) override;

 private:
  // Only the storage owner may change these gates. The mutating flag is atomic
  // because an owner can close it before waiting for the I/O mutex.
  void setWritable(bool writable) { writable_.store(writable); }
  void setEnabled(bool enabled) { enabled_.store(enabled); }
  friend class LittleFsStorageOwner;

  IFileSet& files_;
  ILittleFsStorageLock& lock_;
  std::atomic<bool> writable_;
  std::atomic<bool> enabled_;
};

}  // namespace att
