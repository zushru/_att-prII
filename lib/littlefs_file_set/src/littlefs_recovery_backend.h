#pragma once

#include "recovery_journal.h"
#include <stddef.h>

namespace att {

// POSIX/VFS implementation of the archive-first recovery backend.
// The mount must already exist. Construct with writable=true only inside an
// explicitly authorized physical-service recovery session.
class LittleFsRecoveryBackend final : public IRecoveryBackend {
 public:
  explicit LittleFsRecoveryBackend(const char* mountpoint,
                                   const char* partitionLabel = "spiffs",
                                   bool writable = false);

  LittleFsRecoveryBackend(const LittleFsRecoveryBackend&) = delete;
  LittleFsRecoveryBackend& operator=(const LittleFsRecoveryBackend&) = delete;

  bool valid() const { return valid_; }
  bool writable() const override { return valid_ && writable_; }
  bool capacityAvailable(const RecoveryPlan& plan,
                         uint64_t additionalDataBytes) override;

  bool writeIntent(const RecoveryPlan& plan) override;
  bool findPending(RecoveryPlan& plan, bool& found,
                   bool& ambiguous) override;
  bool ensureArchive(const RecoveryPlan& plan, size_t index) override;
  bool ensureArchiveWithLease(
      const RecoveryPlan& plan, size_t index,
      IRecoveryAuthorization& authorization) override;
  bool verifyArchive(const RecoveryPlan& plan, size_t index) override;
  bool archiveReady(const RecoveryPlan& plan, bool& ready) override;
  bool writeArchiveReady(const RecoveryPlan& plan) override;
  bool activeFileState(uint32_t monthKey, bool& exists,
                       uint64_t& size) override;
  bool activeMatchesArchivePrefix(const RecoveryPlan& plan, size_t index,
                                  uint64_t prefixBytes) override;
  bool replaceActiveFromArchive(const RecoveryPlan& plan, size_t index,
                                 uint64_t retainedFileBytes) override;
  bool replaceActiveFromArchiveWithLease(
      const RecoveryPlan& plan, size_t index, uint64_t retainedFileBytes,
      IRecoveryAuthorization& authorization) override;
  bool removeActiveFile(const RecoveryPlan& plan, size_t index) override;
  bool verifyActiveResult(const RecoveryPlan& plan) override;
  bool completeMarker(const RecoveryPlan& plan, bool& complete) override;
  bool writeCompleteMarker(const RecoveryPlan& plan) override;

 private:
  static const size_t MOUNTPOINT_CAP = 32;
  static const size_t PARTITION_LABEL_CAP = 17;
  static const size_t PATH_CAP = 144;
  static const size_t PLAN_BYTES_MAX =
      4 + 4 + RECOVERY_OPERATION_ID_BYTES + 4 + 8 + 8 + 4 + 32 + 4 +
      RECOVERY_MAX_FILES * 48 + 4;

  char mountpoint_[MOUNTPOINT_CAP];
  char partitionLabel_[PARTITION_LABEL_CAP];
  bool valid_;
  bool writable_;

  bool makePath_(const char* suffix, char* out, size_t capacity) const;
  bool operationPaths_(const uint8_t id[RECOVERY_OPERATION_ID_BYTES],
                       char* operationDir, size_t operationCapacity,
                       char* sourceDir, size_t sourceCapacity) const;
  bool sourcePath_(uint32_t monthKey, char* out, size_t capacity) const;
  bool archivePath_(const RecoveryPlan& plan, uint32_t monthKey,
                    char* out, size_t capacity) const;
  bool ensureDirectory_(const char* path) const;
  bool ensureOperationDirectories_(const RecoveryPlan& plan) const;

  bool readIntent_(const char* path, RecoveryPlan& plan) const;
  bool intentMatches_(const char* path, const RecoveryPlan& plan) const;
  bool serializeMarker_(const char magic[4], const RecoveryPlan& plan,
                        uint8_t out[40]) const;
  bool markerState_(const char* path, const char magic[4],
                    const RecoveryPlan& plan, bool& present) const;
  bool publishImmutable_(const char* finalPath, const char* tempPath,
                         const uint8_t* bytes, size_t length) const;
  bool fileMatchesBytes_(const char* path, const uint8_t* bytes,
                         size_t length) const;
  bool compareFiles_(const char* left, const char* right,
                     uint64_t bytes) const;
  bool fileCrcAndSize_(const char* path, uint64_t& size,
                       uint32_t& crc, uint8_t sha256[32]) const;
  bool activeMatchesArchive_(const RecoveryPlan& plan, size_t index,
                             uint64_t bytes) const;
  bool ensureArchive_(const RecoveryPlan& plan, size_t index,
                      const IRecoveryAuthorization* authorization);
  bool replaceActiveFromArchive_(const RecoveryPlan& plan, size_t index,
                                 uint64_t retainedFileBytes,
                                 const IRecoveryAuthorization* authorization);
  bool intentMatchesPlan_(const RecoveryPlan& plan) const;
  bool validateOperationContents_(const RecoveryPlan& plan,
                                  bool archiveReady, bool complete,
                                  bool requireAllArchives);
  bool archiveSetReady_(const RecoveryPlan& plan);
  bool orphanArtifactsWithoutIntent_(const char* operationDir,
                                     const char* sourceDir) const;
  bool removeIfExists_(const char* path) const;
  bool operationMarkerPath_(const RecoveryPlan& plan, const char* name,
                            char* out, size_t capacity) const;
};

}  // namespace att
