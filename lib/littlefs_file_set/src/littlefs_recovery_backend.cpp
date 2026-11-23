#include "littlefs_recovery_backend.h"
#include "backup_format.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#if !defined(ESP_PLATFORM)
#include <sys/statvfs.h>
#endif
#if defined(ESP_PLATFORM)
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_task_wdt.h>
#endif

namespace att {
namespace {

static_assert(std::numeric_limits<off_t>::is_integer &&
              std::numeric_limits<off_t>::is_signed,
              "POSIX off_t must be a signed integer type");
const uint64_t kMaxFileOffset = static_cast<uint64_t>(
    (std::numeric_limits<off_t>::max)());

void serviceRecoveryWatchdog() {
#if defined(ESP_PLATFORM)
  (void)esp_task_wdt_reset();
#endif
}

void put32(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

void put64(uint8_t* out, uint64_t value) {
  for (size_t i = 0; i < 8; ++i)
    out[i] = (uint8_t)(value >> (8u * i));
}

uint32_t get32(const uint8_t* in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
      ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

uint64_t get64(const uint8_t* in) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
    value |= (uint64_t)in[i] << (8u * i);
  return value;
}

bool validKey(uint32_t key) {
  const uint32_t year = key / 100u;
  const uint32_t month = key % 100u;
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12;
}

bool formatKey(uint32_t key, char* out, size_t capacity) {
  if (!validKey(key) || !out || capacity == 0) return false;
  const int n = snprintf(out, capacity, "%04lu%02lu.bin",
      (unsigned long)(key / 100u), (unsigned long)(key % 100u));
  return n == 10 && (size_t)n < capacity;
}

bool writeAll(int fd, const uint8_t* data, size_t length) {
  size_t done = 0;
  while (done < length) {
    const ssize_t wrote = write(fd, data + done, length - done);
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote <= 0) return false;
    done += (size_t)wrote;
    serviceRecoveryWatchdog();
  }
  return done == length;
}

bool readExact(int fd, uint8_t* data, size_t length) {
  size_t done = 0;
  while (done < length) {
    const ssize_t got = read(fd, data + done, length - done);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    done += (size_t)got;
    serviceRecoveryWatchdog();
  }
  return done == length;
}

bool readAt(int fd, uint8_t* data, size_t length, uint64_t offset) {
  if (offset > kMaxFileOffset ||
      static_cast<uint64_t>(length) > kMaxFileOffset - offset) return false;
  size_t done = 0;
  while (done < length) {
    const uint64_t currentOffset = offset + static_cast<uint64_t>(done);
    const ssize_t got = pread(fd, data + done, length - done,
                              static_cast<off_t>(currentOffset));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    done += (size_t)got;
    serviceRecoveryWatchdog();
  }
  return done == length;
}

bool regularFileSize(const char* path, uint64_t& size, bool& exists) {
  struct stat st;
  if (stat(path, &st) != 0) {
    if (errno == ENOENT) {
      exists = false;
      size = 0;
      return true;
    }
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 0) return false;
  exists = true;
  size = (uint64_t)st.st_size;
  return true;
}

}  // namespace

LittleFsRecoveryBackend::LittleFsRecoveryBackend(const char* mountpoint,
                                                   const char* partitionLabel,
                                                   bool writable)
    : mountpoint_{0}, partitionLabel_{0}, valid_(false),
      writable_(writable) {
  if (!mountpoint || mountpoint[0] != '/' || !partitionLabel ||
      partitionLabel[0] == '\0') return;
  const size_t labelLength = strlen(partitionLabel);
  if (labelLength >= sizeof partitionLabel_) return;
  for (size_t i = 0; i < labelLength; ++i) {
    const char ch = partitionLabel[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return;
  }
  memcpy(partitionLabel_, partitionLabel, labelLength + 1);
  const size_t length = strlen(mountpoint);
  if (length < 2 || length >= sizeof mountpoint_ ||
      mountpoint[length - 1] == '/') return;
  for (size_t i = 1; i < length; ++i) {
    const char ch = mountpoint[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '/'))
      return;
    if (ch == '/' && (i == 1 || mountpoint[i - 1] == '/')) return;
    if (ch == '.' && (i + 1 == length || mountpoint[i + 1] == '/' ||
                      (i > 0 && mountpoint[i - 1] == '/'))) return;
  }
  memcpy(mountpoint_, mountpoint, length + 1);
  valid_ = true;
}

bool LittleFsRecoveryBackend::makePath_(const char* suffix, char* out,
                                        size_t capacity) const {
  if (!valid_ || !suffix || suffix[0] != '/' || !out || capacity == 0)
    return false;
  const int n = snprintf(out, capacity, "%s%s", mountpoint_, suffix);
  return n > 0 && (size_t)n < capacity;
}

bool LittleFsRecoveryBackend::sourcePath_(uint32_t monthKey, char* out,
                                          size_t capacity) const {
  char name[16];
  if (!formatKey(monthKey, name, sizeof name) || !out) return false;
  char suffix[32];
  const int n = snprintf(suffix, sizeof suffix, "/log/%s", name);
  return n > 0 && (size_t)n < sizeof suffix &&
      makePath_(suffix, out, capacity);
}

bool LittleFsRecoveryBackend::operationPaths_(
    const uint8_t id[RECOVERY_OPERATION_ID_BYTES],
    char* operationDir, size_t operationCapacity,
    char* sourceDir, size_t sourceCapacity) const {
  if (!id || !operationDir || !sourceDir) return false;
  char hex[RECOVERY_OPERATION_ID_BYTES * 2 + 1];
  static const char digits[] = "0123456789abcdef";
  uint8_t aggregate = 0;
  for (size_t i = 0; i < RECOVERY_OPERATION_ID_BYTES; ++i) {
    aggregate |= id[i];
    hex[i * 2] = digits[id[i] >> 4];
    hex[i * 2 + 1] = digits[id[i] & 0x0f];
  }
  hex[sizeof hex - 1] = '\0';
  if (aggregate == 0) return false;
  char suffix[64];
  int n = snprintf(suffix, sizeof suffix, "/recovery/%s", hex);
  if (n <= 0 || (size_t)n >= sizeof suffix ||
      !makePath_(suffix, operationDir, operationCapacity)) return false;
  n = snprintf(suffix, sizeof suffix, "/recovery/%s/source", hex);
  return n > 0 && (size_t)n < sizeof suffix &&
      makePath_(suffix, sourceDir, sourceCapacity);
}

bool LittleFsRecoveryBackend::archivePath_(const RecoveryPlan& plan,
                                            uint32_t monthKey, char* out,
                                            size_t capacity) const {
  char operation[PATH_CAP], source[PATH_CAP], name[16], suffix[80];
  if (!operationPaths_(plan.operationId, operation, sizeof operation,
                       source, sizeof source) ||
      !formatKey(monthKey, name, sizeof name)) return false;
  const int n = snprintf(suffix, sizeof suffix, "/recovery/%s/source/%s",
      strrchr(operation, '/') ? strrchr(operation, '/') + 1 : "", name);
  return n > 0 && (size_t)n < sizeof suffix &&
      makePath_(suffix, out, capacity);
}

bool LittleFsRecoveryBackend::ensureDirectory_(const char* path) const {
  struct stat st;
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
  if (errno != ENOENT) return false;
  if (mkdir(path, 0700) == 0) return true;
  if (errno != EEXIST) return false;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool LittleFsRecoveryBackend::ensureOperationDirectories_(
    const RecoveryPlan& plan) const {
  char recovery[PATH_CAP], operation[PATH_CAP], source[PATH_CAP];
  if (!makePath_("/recovery", recovery, sizeof recovery) ||
      !operationPaths_(plan.operationId, operation, sizeof operation,
                       source, sizeof source)) return false;
  return ensureDirectory_(recovery) && ensureDirectory_(operation) &&
      ensureDirectory_(source);
}

bool LittleFsRecoveryBackend::operationMarkerPath_(
    const RecoveryPlan& plan, const char* name, char* out,
    size_t capacity) const {
  if (!name || !out) return false;
  char operation[PATH_CAP], source[PATH_CAP], suffix[80];
  if (!operationPaths_(plan.operationId, operation, sizeof operation,
                       source, sizeof source)) return false;
  const char* id = strrchr(operation, '/');
  if (!id) return false;
  ++id;
  const int n = snprintf(suffix, sizeof suffix, "/recovery/%s/%s", id, name);
  return n > 0 && (size_t)n < sizeof suffix &&
      makePath_(suffix, out, capacity);
}

bool LittleFsRecoveryBackend::readIntent_(const char* path,
                                          RecoveryPlan& plan) const {
  if (!path) return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size >= 88 && (uint64_t)st.st_size <= PLAN_BYTES_MAX;
  uint8_t header[84];
  if (ok) ok = readExact(fd, header, sizeof header);
  if (!ok || memcmp(header, "ATRI", 4) != 0) {
    (void)close(fd);
    return false;
  }

  const uint32_t count = get32(header + 24);
  if (count == 0 || count > RECOVERY_MAX_FILES) {
    (void)close(fd);
    return false;
  }
  const size_t expected = 84u + (size_t)count * 48u + 4u;
  if ((uint64_t)st.st_size != expected) {
    (void)close(fd);
    return false;
  }

  memset(&plan, 0, sizeof plan);
  plan.version = get32(header + 4);
  memcpy(plan.operationId, header + 8, sizeof plan.operationId);
  plan.sourceCount = count;
  plan.sourceTotalBytes = get64(header + 28);
  plan.retainedBytes = get64(header + 36);
  plan.sourceManifestCrc32 = get32(header + 44);
  memcpy(plan.sourceManifestSha256, header + 48,
         sizeof plan.sourceManifestSha256);
  plan.planCrc32 = get32(header + 80);

  Crc32 fileCrc;
  fileCrc.add(header, sizeof header);
  uint8_t entry[48];
  for (size_t i = 0; ok && i < count; ++i) {
    ok = readExact(fd, entry, sizeof entry);
    if (!ok) break;
    fileCrc.add(entry, sizeof entry);
    plan.source[i].monthKey = get32(entry);
    plan.source[i].byteSize = get64(entry + 4);
    plan.source[i].contentCrc32 = get32(entry + 12);
    memcpy(plan.source[i].contentSha256, entry + 16, 32);
  }

  uint8_t trailer[4];
  if (ok) ok = readExact(fd, trailer, sizeof trailer);
  if (ok) ok = get32(trailer) == fileCrc.value();
  struct stat after;
  if (ok) ok = fstat(fd, &after) == 0 && S_ISREG(after.st_mode) &&
      after.st_size == st.st_size;
  if (close(fd) != 0) ok = false;
  return ok && RecoveryJournal::validatePlan(plan);
}

bool LittleFsRecoveryBackend::intentMatches_(
    const char* path, const RecoveryPlan& plan) const {
  if (!path || !RecoveryJournal::validatePlan(plan)) return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size >= 88 &&
      (uint64_t)st.st_size == 84u + plan.sourceCount * 48u + 4u;
  uint8_t header[84];
  if (ok) ok = readExact(fd, header, sizeof header);
  if (ok) {
    ok = memcmp(header, "ATRI", 4) == 0 &&
        get32(header + 4) == plan.version &&
        memcmp(header + 8, plan.operationId, sizeof plan.operationId) == 0 &&
        get32(header + 24) == plan.sourceCount &&
        get64(header + 28) == plan.sourceTotalBytes &&
        get64(header + 36) == plan.retainedBytes &&
        get32(header + 44) == plan.sourceManifestCrc32 &&
        memcmp(header + 48, plan.sourceManifestSha256, 32) == 0 &&
        get32(header + 80) == plan.planCrc32;
  }

  Crc32 fileCrc;
  if (ok) fileCrc.add(header, sizeof header);
  uint8_t entry[48];
  for (size_t i = 0; ok && i < plan.sourceCount; ++i) {
    ok = readExact(fd, entry, sizeof entry);
    if (!ok) break;
    fileCrc.add(entry, sizeof entry);
    ok = get32(entry) == plan.source[i].monthKey &&
        get64(entry + 4) == plan.source[i].byteSize &&
        get32(entry + 12) == plan.source[i].contentCrc32 &&
        memcmp(entry + 16, plan.source[i].contentSha256, 32) == 0;
  }

  uint8_t trailer[4];
  if (ok) ok = readExact(fd, trailer, sizeof trailer) &&
      get32(trailer) == fileCrc.value();
  struct stat after;
  if (ok) ok = fstat(fd, &after) == 0 && S_ISREG(after.st_mode) &&
      after.st_size == st.st_size;
  if (close(fd) != 0) ok = false;
  return ok;
}

bool LittleFsRecoveryBackend::serializeMarker_(
    const char magic[4], const RecoveryPlan& plan, uint8_t out[40]) const {
  if (!magic || !out || !RecoveryJournal::validatePlan(plan)) return false;
  memcpy(out, magic, 4);
  put32(out + 4, RECOVERY_JOURNAL_VERSION);
  memcpy(out + 8, plan.operationId, sizeof plan.operationId);
  put32(out + 24, plan.planCrc32);
  put64(out + 28, plan.retainedBytes);
  put32(out + 36, crc32(out, 36));
  return true;
}

bool LittleFsRecoveryBackend::fileMatchesBytes_(
    const char* path, const uint8_t* bytes, size_t length) const {
  if (!path || (length && !bytes)) return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size >= 0 && (uint64_t)st.st_size == length;
  uint8_t buffer[128];
  size_t offset = 0;
  while (ok && offset < length) {
    const size_t chunk = length - offset > sizeof buffer
        ? sizeof buffer : length - offset;
    if (!readAt(fd, buffer, chunk, offset) ||
        memcmp(buffer, bytes + offset, chunk) != 0)
      ok = false;
    offset += chunk;
  }
  struct stat after;
  if (ok && (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
             after.st_size != st.st_size)) ok = false;
  if (close(fd) != 0) ok = false;
  return ok;
}

bool LittleFsRecoveryBackend::publishImmutable_(
    const char* finalPath, const char* tempPath,
    const uint8_t* bytes, size_t length) const {
  if (!finalPath || !tempPath || !bytes || length == 0) return false;
  uint64_t ignoredSize = 0;
  bool exists = false;
  if (!regularFileSize(finalPath, ignoredSize, exists)) return false;
  if (exists) return fileMatchesBytes_(finalPath, bytes, length);

  if (!removeIfExists_(tempPath)) return false;
  int fd = open(tempPath, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  bool ok = writeAll(fd, bytes, length);
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok || !fileMatchesBytes_(tempPath, bytes, length)) {
    (void)removeIfExists_(tempPath);
    return false;
  }
  if (rename(tempPath, finalPath) != 0) {
    if (fileMatchesBytes_(finalPath, bytes, length)) {
      (void)removeIfExists_(tempPath);
      return true;
    }
    return false;
  }
  return fileMatchesBytes_(finalPath, bytes, length);
}

bool LittleFsRecoveryBackend::removeIfExists_(const char* path) const {
  if (!path) return false;
  if (unlink(path) == 0 || errno == ENOENT) return true;
  return false;
}

bool LittleFsRecoveryBackend::capacityAvailable(
    const RecoveryPlan& plan, uint64_t additionalDataBytes) {
  if (!valid_ || !RecoveryJournal::validatePlan(plan)) return false;
#if defined(ESP_PLATFORM)
  size_t total = 0;
  size_t used = 0;
  if (esp_littlefs_info(partitionLabel_, &total, &used) != ESP_OK ||
      used > total)
    return false;
  const uint64_t freeBytes = (uint64_t)(total - used);
#else
  struct statvfs space;
  if (statvfs(mountpoint_, &space) != 0 ||
      space.f_frsize == 0 ||
      space.f_bavail > UINT64_MAX / space.f_frsize)
    return false;
  const uint64_t freeBytes = (uint64_t)space.f_bavail * space.f_frsize;
#endif
  const uint64_t reserve = 65536u +
      (uint64_t)plan.sourceCount * 512u;
  if (additionalDataBytes > UINT64_MAX - reserve) return false;
  const uint64_t required = additionalDataBytes + reserve;
  return freeBytes >= required;
}

bool LittleFsRecoveryBackend::writeIntent(const RecoveryPlan& plan) {
  char operation[PATH_CAP], source[PATH_CAP];
  if (!writable() || !RecoveryJournal::validatePlan(plan) ||
      !ensureOperationDirectories_(plan) ||
      !operationPaths_(plan.operationId, operation, sizeof operation,
                       source, sizeof source) ||
      orphanArtifactsWithoutIntent_(operation, source)) return false;

  char finalPath[PATH_CAP], tempPath[PATH_CAP];
  if (!operationMarkerPath_(plan, "intent.bin", finalPath, sizeof finalPath) ||
      !operationMarkerPath_(plan, "intent.tmp", tempPath, sizeof tempPath))
    return false;
  uint64_t existingSize = 0;
  bool exists = false;
  if (!regularFileSize(finalPath, existingSize, exists) || exists)
    return false;

  if (!removeIfExists_(tempPath)) return false;
  int fd = open(tempPath, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;

  uint8_t header[84] = {};
  memcpy(header, "ATRI", 4);
  put32(header + 4, plan.version);
  memcpy(header + 8, plan.operationId, sizeof plan.operationId);
  put32(header + 24, (uint32_t)plan.sourceCount);
  put64(header + 28, plan.sourceTotalBytes);
  put64(header + 36, plan.retainedBytes);
  put32(header + 44, plan.sourceManifestCrc32);
  memcpy(header + 48, plan.sourceManifestSha256,
         sizeof plan.sourceManifestSha256);
  put32(header + 80, plan.planCrc32);

  Crc32 fileCrc;
  bool ok = writeAll(fd, header, sizeof header);
  if (ok) fileCrc.add(header, sizeof header);
  uint8_t entry[48];
  for (size_t i = 0; ok && i < plan.sourceCount; ++i) {
    put32(entry, plan.source[i].monthKey);
    put64(entry + 4, plan.source[i].byteSize);
    put32(entry + 12, plan.source[i].contentCrc32);
    memcpy(entry + 16, plan.source[i].contentSha256, 32);
    ok = writeAll(fd, entry, sizeof entry);
    if (ok) fileCrc.add(entry, sizeof entry);
  }
  uint8_t trailer[4];
  put32(trailer, fileCrc.value());
  if (ok) ok = writeAll(fd, trailer, sizeof trailer);
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok || !intentMatches_(tempPath, plan)) {
    (void)removeIfExists_(tempPath);
    return false;
  }
  if (rename(tempPath, finalPath) != 0) {
    if (!intentMatches_(finalPath, plan)) return false;
    (void)removeIfExists_(tempPath);
    return true;
  }
  return intentMatches_(finalPath, plan);
}

bool LittleFsRecoveryBackend::markerState_(
    const char* path, const char magic[4], const RecoveryPlan& plan,
    bool& present) const {
  present = false;
  if (!path || !magic) return false;
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return errno == ENOENT;
  present = true;

  struct stat before;
  bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
      before.st_size == 40;
  uint8_t expected[40] = {}, actual[40] = {};
  if (ok) ok = serializeMarker_(magic, plan, expected);
  if (ok) ok = readExact(fd, actual, sizeof actual);
  struct stat after;
  if (ok) ok = fstat(fd, &after) == 0 && S_ISREG(after.st_mode) &&
      after.st_size == before.st_size;
  if (close(fd) != 0) ok = false;
  return ok && memcmp(actual, expected, sizeof actual) == 0;
}

bool LittleFsRecoveryBackend::archiveReady(const RecoveryPlan& plan,
                                            bool& ready) {
  ready = false;
  char path[PATH_CAP];
  if (!operationMarkerPath_(plan, "archive.ready", path, sizeof path))
    return false;
  return markerState_(path, "ATRA", plan, ready);
}

bool LittleFsRecoveryBackend::writeArchiveReady(const RecoveryPlan& plan) {
  bool complete = false;
  if (!writable() || !intentMatchesPlan_(plan) ||
      !completeMarker(plan, complete) || complete ||
      !validateOperationContents_(plan, false, false, true)) return false;
  // Verify both sides immediately before the authority marker is published.
  // This catches source changes between the initial inventory and the end of
  // the multi-file archive pass; the recovery service still requires exclusive
  // filesystem ownership to close the final check-to-marker race.
  for (size_t i = 0; i < plan.sourceCount; ++i) {
    bool exists = false;
    uint64_t size = 0;
    if (!activeFileState(plan.source[i].monthKey, exists, size) ||
        !exists || size != plan.source[i].byteSize ||
        !activeMatchesArchivePrefix(plan, i, size))
      return false;
  }
  char finalPath[PATH_CAP], tempPath[PATH_CAP];
  uint8_t bytes[40];
  if (!operationMarkerPath_(plan, "archive.ready", finalPath, sizeof finalPath) ||
      !operationMarkerPath_(plan, "archive.tmp", tempPath, sizeof tempPath) ||
      !serializeMarker_("ATRA", plan, bytes)) return false;
  return publishImmutable_(finalPath, tempPath, bytes, sizeof bytes);
}

bool LittleFsRecoveryBackend::fileCrcAndSize_(
    const char* path, uint64_t& size, uint32_t& crc,
    uint8_t sha256[32]) const {
  size = 0;
  crc = 0;
  if (!path || !sha256) return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0;
  if (ok) size = (uint64_t)st.st_size;
  Crc32 checksum;
  Sha256 strongChecksum;
  uint8_t buffer[256];
  uint64_t offset = 0;
  while (ok && offset < size) {
    const size_t chunk = (size - offset) > sizeof buffer
        ? sizeof buffer : (size_t)(size - offset);
    if (!readAt(fd, buffer, chunk, offset)) {
      ok = false;
      break;
    }
    checksum.add(buffer, chunk);
    strongChecksum.update(buffer, chunk);
    offset += chunk;
  }
  struct stat after;
  if (ok && (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
             after.st_size != st.st_size)) ok = false;
  if (close(fd) != 0) ok = false;
  if (ok) {
    crc = checksum.value();
    strongChecksum.finish(sha256);
  }
  return ok;
}

bool LittleFsRecoveryBackend::compareFiles_(
    const char* left, const char* right, uint64_t bytes) const {
  if (!left || !right) return false;
  int a = open(left, O_RDONLY);
  if (a < 0) return false;
  int b = open(right, O_RDONLY);
  if (b < 0) {
    close(a);
    return false;
  }
  struct stat sa, sb;
  bool ok = fstat(a, &sa) == 0 && fstat(b, &sb) == 0 &&
      S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode) &&
      sa.st_size >= 0 && sb.st_size >= 0 &&
      (uint64_t)sa.st_size >= bytes && (uint64_t)sb.st_size >= bytes;
  uint8_t ba[256], bb[256];
  uint64_t offset = 0;
  while (ok && offset < bytes) {
    const size_t chunk = bytes - offset > sizeof ba
        ? sizeof ba : (size_t)(bytes - offset);
    if (!readAt(a, ba, chunk, offset) || !readAt(b, bb, chunk, offset) ||
        memcmp(ba, bb, chunk) != 0)
      ok = false;
    offset += chunk;
  }
  struct stat afterA, afterB;
  if (ok && (fstat(a, &afterA) != 0 || fstat(b, &afterB) != 0 ||
             !S_ISREG(afterA.st_mode) || !S_ISREG(afterB.st_mode) ||
             afterA.st_size != sa.st_size || afterB.st_size != sb.st_size))
    ok = false;
  if (close(a) != 0) ok = false;
  if (close(b) != 0) ok = false;
  return ok;
}

bool LittleFsRecoveryBackend::verifyArchive(const RecoveryPlan& plan,
                                             size_t index) {
  if (!RecoveryJournal::validatePlan(plan) || index >= plan.sourceCount)
    return false;
  char path[PATH_CAP];
  uint64_t size = 0;
  uint32_t crc = 0;
  uint8_t sha[32];
  return archivePath_(plan, plan.source[index].monthKey, path, sizeof path) &&
      fileCrcAndSize_(path, size, crc, sha) &&
      size == plan.source[index].byteSize &&
      crc == plan.source[index].contentCrc32 &&
      memcmp(sha, plan.source[index].contentSha256, sizeof sha) == 0;
}

bool LittleFsRecoveryBackend::intentMatchesPlan_(
    const RecoveryPlan& plan) const {
  char path[PATH_CAP];
  return RecoveryJournal::validatePlan(plan) &&
      operationMarkerPath_(plan, "intent.bin", path, sizeof path) &&
      intentMatches_(path, plan);
}

bool LittleFsRecoveryBackend::validateOperationContents_(
    const RecoveryPlan& plan, bool archiveCommitted, bool complete,
    bool requireAllArchives) {
  if (!RecoveryJournal::validatePlan(plan) ||
      (complete && !archiveCommitted)) return false;

  char operation[PATH_CAP], sourceDir[PATH_CAP];
  if (!operationPaths_(plan.operationId, operation, sizeof operation,
                       sourceDir, sizeof sourceDir)) return false;
  DIR* operationDir = opendir(operation);
  if (!operationDir) return false;

  bool sawSource = false;
  bool sawIntent = false;
  bool sawArchiveReady = false;
  bool sawComplete = false;
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent* item = readdir(operationDir);
    serviceRecoveryWatchdog();
    if (!item) {
      if (errno != 0) ok = false;
      break;
    }
    const char* name = item->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    char path[PATH_CAP];
    const int n = snprintf(path, sizeof path, "%s/%s", operation, name);
    uint64_t size = 0;
    bool exists = false;
    if (n <= 0 || (size_t)n >= sizeof path) {
      ok = false;
      break;
    }

    if (strcmp(name, "source") == 0) {
      struct stat st;
      if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ok = false;
        break;
      }
      sawSource = true;
    } else if (strcmp(name, "intent.bin") == 0) {
      if (!regularFileSize(path, size, exists) || !exists || size == 0) {
        ok = false;
        break;
      }
      sawIntent = true;
    } else if (strcmp(name, "archive.ready") == 0) {
      if (!archiveCommitted ||
          !regularFileSize(path, size, exists) || !exists) {
        ok = false;
        break;
      }
      sawArchiveReady = true;
    } else if (strcmp(name, "complete.ok") == 0) {
      if (!complete ||
          !regularFileSize(path, size, exists) || !exists) {
        ok = false;
        break;
      }
      sawComplete = true;
    } else if (strcmp(name, "archive.tmp") == 0) {
      if (archiveCommitted ||
          !regularFileSize(path, size, exists) || !exists) {
        ok = false;
        break;
      }
    } else if (strcmp(name, "complete.tmp") == 0) {
      if (!archiveCommitted || complete ||
          !regularFileSize(path, size, exists) || !exists) {
        ok = false;
        break;
      }
    } else {
      // intent.tmp is only valid before intent.bin exists. Unknown names or
      // a second intent artifact are contradictory once a valid intent exists.
      ok = false;
      break;
    }
  }
  if (closedir(operationDir) != 0) ok = false;
  if (!ok || !sawSource || !sawIntent ||
      sawArchiveReady != archiveCommitted || sawComplete != complete)
    return false;

  bool archived[RECOVERY_MAX_FILES] = {};
  bool staging[RECOVERY_MAX_FILES] = {};
  DIR* source = opendir(sourceDir);
  if (!source) return false;
  for (;;) {
    errno = 0;
    struct dirent* item = readdir(source);
    serviceRecoveryWatchdog();
    if (!item) {
      if (errno != 0) ok = false;
      break;
    }
    const char* name = item->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    const size_t length = strlen(name);
    if (length != 10 ||
        (memcmp(name + 6, ".bin", 4) != 0 &&
         memcmp(name + 6, ".tmp", 4) != 0)) {
      ok = false;
      break;
    }

    uint32_t key = 0;
    for (size_t i = 0; i < 6; ++i) {
      if (name[i] < '0' || name[i] > '9') {
        ok = false;
        break;
      }
      key = key * 10u + static_cast<uint32_t>(name[i] - '0');
    }
    if (!ok || !validKey(key)) { ok = false; break; }

    size_t index = 0;
    while (index < plan.sourceCount && plan.source[index].monthKey != key)
      ++index;
    if (index == plan.sourceCount) { ok = false; break; }

    char path[PATH_CAP];
    const int n = snprintf(path, sizeof path, "%s/%s", sourceDir, name);
    uint64_t size = 0;
    bool exists = false;
    if (n <= 0 || (size_t)n >= sizeof path ||
        !regularFileSize(path, size, exists) || !exists) {
      ok = false;
      break;
    }

    const bool isArchive = memcmp(name + 6, ".bin", 4) == 0;
    if (isArchive) {
      if (archived[index] || staging[index]) { ok = false; break; }
      archived[index] = true;
    } else {
      if (archiveCommitted || requireAllArchives ||
          staging[index] || archived[index]) {
        ok = false;
        break;
      }
      staging[index] = true;
    }
  }
  if (closedir(source) != 0) ok = false;
  if (!ok) return false;

  for (size_t i = 0; i < plan.sourceCount; ++i) {
    if ((archiveCommitted || requireAllArchives) &&
        (!archived[i] || staging[i])) return false;
    if (archiveCommitted || requireAllArchives) {
      if (!verifyArchive(plan, i)) return false;
    }
  }
  return true;
}

bool LittleFsRecoveryBackend::archiveSetReady_(
    const RecoveryPlan& plan) {
  bool ready = false;
  bool complete = false;
  if (!intentMatchesPlan_(plan) || !archiveReady(plan, ready) || !ready ||
      !completeMarker(plan, complete) || complete)
    return false;
  return validateOperationContents_(plan, true, false, true);
}

bool LittleFsRecoveryBackend::ensureArchive(const RecoveryPlan& plan,
                                             size_t index) {
  return ensureArchive_(plan, index, nullptr);
}

bool LittleFsRecoveryBackend::ensureArchiveWithLease(
    const RecoveryPlan& plan, size_t index,
    IRecoveryAuthorization& authorization) {
  return ensureArchive_(plan, index, &authorization);
}

bool LittleFsRecoveryBackend::ensureArchive_(
    const RecoveryPlan& plan, size_t index,
    const IRecoveryAuthorization* authorization) {
  if ((authorization && !authorization->continueAuthorized()) ||
      !writable() || !RecoveryJournal::validatePlan(plan) ||
      index >= plan.sourceCount || !intentMatchesPlan_(plan)) return false;
  bool ready = false;
  bool complete = false;
  if (!archiveReady(plan, ready) || ready ||
      !completeMarker(plan, complete) || complete) return false;
  const MonthFileInventoryEntry& entry = plan.source[index];
  char source[PATH_CAP], archive[PATH_CAP], temp[PATH_CAP];
  if (!sourcePath_(entry.monthKey, source, sizeof source) ||
      !archivePath_(plan, entry.monthKey, archive, sizeof archive))
    return false;
  char tempName[24];
  const int tempLen = snprintf(tempName, sizeof tempName, "%04lu%02lu.tmp",
      (unsigned long)(entry.monthKey / 100u),
      (unsigned long)(entry.monthKey % 100u));
  if (tempLen <= 0 || (size_t)tempLen >= sizeof tempName) return false;
  char operation[PATH_CAP], sourceDir[PATH_CAP], suffix[80];
  if (!operationPaths_(plan.operationId, operation, sizeof operation,
                       sourceDir, sizeof sourceDir)) return false;
  const char* id = strrchr(operation, '/');
  if (!id) return false;
  const int suffixLen = snprintf(suffix, sizeof suffix, "/recovery/%s/source/%s",
      id + 1, tempName);
  if (suffixLen <= 0 || (size_t)suffixLen >= sizeof suffix ||
      !makePath_(suffix, temp, sizeof temp)) return false;

  uint64_t archiveSize = 0;
  bool archiveExists = false;
  if (!regularFileSize(archive, archiveSize, archiveExists)) return false;
  if (archiveExists && verifyArchive(plan, index)) return true;
  if (!removeIfExists_(archive) || !removeIfExists_(temp)) return false;

  int input = open(source, O_RDONLY);
  if (input < 0) return false;
  struct stat inputStat;
  bool ok = fstat(input, &inputStat) == 0 && S_ISREG(inputStat.st_mode) &&
      inputStat.st_size >= 0 &&
      (uint64_t)inputStat.st_size == entry.byteSize;
  int output = -1;
  if (ok) output = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (output < 0) ok = false;

  Crc32 checksum;
  Sha256 strongChecksum;
  uint8_t buffer[256];
  uint64_t offset = 0;
  while (ok && offset < entry.byteSize) {
    const size_t chunk = entry.byteSize - offset > sizeof buffer
        ? sizeof buffer : (size_t)(entry.byteSize - offset);
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
    if (!readAt(input, buffer, chunk, offset)) {
      ok = false;
      break;
    }
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
    if (!writeAll(output, buffer, chunk)) {
      ok = false;
      break;
    }
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
    checksum.add(buffer, chunk);
    strongChecksum.update(buffer, chunk);
    offset += chunk;
  }
  struct stat inputAfter;
  if (ok && (fstat(input, &inputAfter) != 0 ||
             !S_ISREG(inputAfter.st_mode) ||
             inputAfter.st_size != inputStat.st_size)) ok = false;
  if (authorization && !authorization->continueAuthorized()) ok = false;
  uint8_t copiedSha[32];
  strongChecksum.finish(copiedSha);
  if (ok && (checksum.value() != entry.contentCrc32 ||
      memcmp(copiedSha, entry.contentSha256, sizeof copiedSha) != 0))
    ok = false;
  if (ok && fsync(output) != 0) ok = false;
  if (output >= 0 && close(output) != 0) ok = false;
  if (close(input) != 0) ok = false;
  if (!ok) {
    (void)removeIfExists_(temp);
    return false;
  }

  uint64_t tempSize = 0;
  uint32_t tempCrc = 0;
  uint8_t tempSha[32];
  if (!fileCrcAndSize_(temp, tempSize, tempCrc, tempSha) ||
      tempSize != entry.byteSize || tempCrc != entry.contentCrc32 ||
      memcmp(tempSha, entry.contentSha256, sizeof tempSha) != 0 ||
      !compareFiles_(source, temp, entry.byteSize)) {
    (void)removeIfExists_(temp);
    return false;
  }
  if (authorization && !authorization->continueAuthorized()) {
    (void)removeIfExists_(temp);
    return false;
  }
  if (rename(temp, archive) != 0) {
    if (!verifyArchive(plan, index)) return false;
    (void)removeIfExists_(temp);
  }
  if (authorization && !authorization->continueAuthorized()) return false;
  bool sourceExists = false;
  uint64_t sourceSize = 0;
  const bool verified = verifyArchive(plan, index) &&
      activeFileState(entry.monthKey, sourceExists, sourceSize) &&
      sourceExists && sourceSize == entry.byteSize &&
      compareFiles_(source, archive, entry.byteSize);
  return verified &&
      (!authorization || authorization->continueAuthorized());
}

bool LittleFsRecoveryBackend::activeFileState(uint32_t monthKey,
                                               bool& exists,
                                               uint64_t& size) {
  exists = false;
  size = 0;
  char path[PATH_CAP];
  return sourcePath_(monthKey, path, sizeof path) &&
      regularFileSize(path, size, exists);
}

bool LittleFsRecoveryBackend::activeMatchesArchive_(
    const RecoveryPlan& plan, size_t index, uint64_t bytes) const {
  if (index >= plan.sourceCount || bytes > plan.source[index].byteSize)
    return false;
  char active[PATH_CAP], archive[PATH_CAP];
  return sourcePath_(plan.source[index].monthKey, active, sizeof active) &&
      archivePath_(plan, plan.source[index].monthKey, archive, sizeof archive) &&
      compareFiles_(active, archive, bytes);
}

bool LittleFsRecoveryBackend::activeMatchesArchivePrefix(
    const RecoveryPlan& plan, size_t index, uint64_t prefixBytes) {
  return RecoveryJournal::validatePlan(plan) &&
      activeMatchesArchive_(plan, index, prefixBytes);
}

bool LittleFsRecoveryBackend::replaceActiveFromArchive(
    const RecoveryPlan& plan, size_t index, uint64_t retainedFileBytes) {
  return replaceActiveFromArchive_(plan, index, retainedFileBytes, nullptr);
}

bool LittleFsRecoveryBackend::replaceActiveFromArchiveWithLease(
    const RecoveryPlan& plan, size_t index, uint64_t retainedFileBytes,
    IRecoveryAuthorization& authorization) {
  return replaceActiveFromArchive_(
      plan, index, retainedFileBytes, &authorization);
}

bool LittleFsRecoveryBackend::replaceActiveFromArchive_(
    const RecoveryPlan& plan, size_t index, uint64_t retainedFileBytes,
    const IRecoveryAuthorization* authorization) {
  if ((authorization && !authorization->continueAuthorized()) ||
      !writable() || !RecoveryJournal::validatePlan(plan) ||
      index >= plan.sourceCount ||
      retainedFileBytes > plan.source[index].byteSize ||
      retainedFileBytes > kMaxFileOffset ||
      !archiveSetReady_(plan))
    return false;

  char active[PATH_CAP], archive[PATH_CAP];
  if (!sourcePath_(plan.source[index].monthKey, active, sizeof active) ||
      !archivePath_(plan, plan.source[index].monthKey, archive, sizeof archive))
    return false;

  // The archive-ready marker is already committed before this is callable.
  // Rewriting the active path is restartable: after any interruption, the
  // verified archive remains authoritative and this routine can be repeated.
  int input = open(archive, O_RDONLY);
  if (input < 0) return false;
  if (authorization && !authorization->continueAuthorized()) {
    (void)close(input);
    return false;
  }
  // Sample immediately before O_TRUNC, then at every chunk boundary.
  int output = open(active, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (output < 0) {
    (void)close(input);
    return false;
  }

  bool ok = true;
  uint8_t buffer[256];
  uint64_t offset = 0;
  while (ok && offset < retainedFileBytes) {
    const size_t chunk = retainedFileBytes - offset > sizeof buffer
        ? sizeof buffer : (size_t)(retainedFileBytes - offset);
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
    if (!readAt(input, buffer, chunk, offset)) {
      ok = false;
      break;
    }
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
    if (!writeAll(output, buffer, chunk)) {
      ok = false;
      break;
    }
    offset += chunk;
    if (authorization && !authorization->continueAuthorized()) {
      ok = false;
      break;
    }
  }
  if (authorization && !authorization->continueAuthorized()) ok = false;
  if (ok && fsync(output) != 0) ok = false;
  if (close(output) != 0) ok = false;
  if (close(input) != 0) ok = false;
  if (!ok || (authorization && !authorization->continueAuthorized()))
    return false;

  bool exists = false;
  uint64_t activeSize = 0;
  const bool verified =
      activeFileState(plan.source[index].monthKey, exists, activeSize) &&
      exists && activeSize == retainedFileBytes &&
      activeMatchesArchive_(plan, index, retainedFileBytes);
  return verified &&
      (!authorization || authorization->continueAuthorized());
}

bool LittleFsRecoveryBackend::removeActiveFile(const RecoveryPlan& plan,
                                                size_t index) {
  if (!writable() || !RecoveryJournal::validatePlan(plan) ||
      index >= plan.sourceCount || !archiveSetReady_(plan)) return false;
  bool exists = false;
  uint64_t size = 0;
  if (!activeFileState(plan.source[index].monthKey, exists, size))
    return false;
  if (!exists) return true;
  // Once archive.ready is committed, the exact original bytes are preserved
  // under /recovery. An active file can now be removed even if a previous
  // interrupted rewrite left it shortened or partially written.
  char path[PATH_CAP];
  if (!sourcePath_(plan.source[index].monthKey, path, sizeof path) ||
      unlink(path) != 0) return false;
  return activeFileState(plan.source[index].monthKey, exists, size) &&
      !exists;
}

bool LittleFsRecoveryBackend::verifyActiveResult(const RecoveryPlan& plan) {
  if (!RecoveryJournal::validatePlan(plan)) return false;
  uint64_t remaining = plan.retainedBytes;
  size_t expectedCount = 0;
  for (size_t i = 0; i < plan.sourceCount; ++i) {
    const uint64_t keep = remaining < plan.source[i].byteSize
        ? remaining : plan.source[i].byteSize;
    remaining -= keep;
    if (keep != 0) ++expectedCount;
    bool exists = false;
    uint64_t size = 0;
    if (!activeFileState(plan.source[i].monthKey, exists, size))
      return false;
    if (keep == 0) {
      if (exists) return false;
    } else if (!exists || size != keep ||
               !activeMatchesArchive_(plan, i, keep)) {
      return false;
    }
  }
  if (remaining != 0) return false;

  char logDir[PATH_CAP];
  if (!makePath_("/log", logDir, sizeof logDir)) return false;
  DIR* dir = opendir(logDir);
  if (!dir) return expectedCount == 0 && errno == ENOENT;
  size_t actualCount = 0;
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent* item = readdir(dir);
    serviceRecoveryWatchdog();
    if (!item) {
      if (errno != 0) ok = false;
      break;
    }
    if (strcmp(item->d_name, ".") == 0 ||
        strcmp(item->d_name, "..") == 0) continue;
    if (strlen(item->d_name) != 10 ||
        memcmp(item->d_name + 6, ".bin", 4) != 0) {
      ok = false;
      break;
    }
    uint32_t key = 0;
    for (size_t i = 0; i < 6; ++i) {
      if (item->d_name[i] < '0' || item->d_name[i] > '9') {
        ok = false;
        break;
      }
      key = key * 10u + (uint32_t)(item->d_name[i] - '0');
    }
    if (!ok || !validKey(key)) { ok = false; break; }
    bool planned = false;
    for (size_t i = 0; i < plan.sourceCount; ++i) {
      uint64_t remainingForEntry = plan.retainedBytes;
      for (size_t j = 0; j < i; ++j)
        remainingForEntry -= remainingForEntry < plan.source[j].byteSize
            ? remainingForEntry : plan.source[j].byteSize;
      const uint64_t keep = remainingForEntry < plan.source[i].byteSize
          ? remainingForEntry : plan.source[i].byteSize;
      if (plan.source[i].monthKey == key && keep != 0) {
        planned = true;
        break;
      }
    }
    if (!planned) { ok = false; break; }
    ++actualCount;
  }
  if (closedir(dir) != 0) ok = false;
  return ok && actualCount == expectedCount;
}

bool LittleFsRecoveryBackend::completeMarker(const RecoveryPlan& plan,
                                              bool& complete) {
  complete = false;
  char path[PATH_CAP];
  return operationMarkerPath_(plan, "complete.ok", path, sizeof path) &&
      markerState_(path, "ATRC", plan, complete);
}

bool LittleFsRecoveryBackend::writeCompleteMarker(const RecoveryPlan& plan) {
  if (!writable() || !archiveSetReady_(plan) ||
      !verifyActiveResult(plan)) return false;
  char finalPath[PATH_CAP], tempPath[PATH_CAP];
  uint8_t bytes[40];
  if (!operationMarkerPath_(plan, "complete.ok", finalPath, sizeof finalPath) ||
      !operationMarkerPath_(plan, "complete.tmp", tempPath, sizeof tempPath) ||
      !serializeMarker_("ATRC", plan, bytes)) return false;
  return publishImmutable_(finalPath, tempPath, bytes, sizeof bytes);
}

bool LittleFsRecoveryBackend::orphanArtifactsWithoutIntent_(
    const char* operationDir, const char* sourceDir) const {
  if (!operationDir || !sourceDir) return true;
  DIR* operation = opendir(operationDir);
  if (!operation) return true;

  bool safe = true;
  bool sourceSeen = false;
  for (;;) {
    errno = 0;
    struct dirent* item = readdir(operation);
    serviceRecoveryWatchdog();
    if (!item) {
      if (errno != 0) safe = false;
      break;
    }
    if (strcmp(item->d_name, ".") == 0 ||
        strcmp(item->d_name, "..") == 0) continue;
    if (strcmp(item->d_name, "source") == 0) {
      struct stat st;
      if (stat(sourceDir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        safe = false;
        break;
      }
      sourceSeen = true;
    } else if (strcmp(item->d_name, "intent.tmp") == 0) {
      char path[PATH_CAP];
      const int n = snprintf(path, sizeof path, "%s/intent.tmp", operationDir);
      uint64_t size = 0;
      bool exists = false;
      if (n <= 0 || (size_t)n >= sizeof path ||
          !regularFileSize(path, size, exists) || !exists) {
        safe = false;
        break;
      }
    } else {
      // A phase marker, archive, or unknown entry without intent means the
      // journal's authority chain is incomplete. Never start a new operation.
      safe = false;
      break;
    }
  }
  if (closedir(operation) != 0) safe = false;
  if (!safe) return true;

  if (sourceSeen) {
    DIR* source = opendir(sourceDir);
    if (!source) return true;
    for (;;) {
      errno = 0;
      struct dirent* item = readdir(source);
      serviceRecoveryWatchdog();
      if (!item) {
        if (errno != 0) safe = false;
        break;
      }
      if (strcmp(item->d_name, ".") != 0 &&
          strcmp(item->d_name, "..") != 0) {
        safe = false;
        break;
      }
    }
    if (closedir(source) != 0) safe = false;
  }
  return !safe;
}

bool LittleFsRecoveryBackend::findPending(RecoveryPlan& plan, bool& found,
                                           bool& ambiguous) {
  found = false;
  ambiguous = false;
  if (!valid_) return false;
  char root[PATH_CAP];
  if (!makePath_("/recovery", root, sizeof root)) return false;
  struct stat rootStat;
  if (stat(root, &rootStat) != 0) return errno == ENOENT;
  if (!S_ISDIR(rootStat.st_mode)) return false;
  DIR* dir = opendir(root);
  if (!dir) return false;

  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent* item = readdir(dir);
    serviceRecoveryWatchdog();
    if (!item) {
      if (errno != 0) ok = false;
      break;
    }
    if (strcmp(item->d_name, ".") == 0 ||
        strcmp(item->d_name, "..") == 0) continue;
    if (strlen(item->d_name) != RECOVERY_OPERATION_ID_BYTES * 2) {
      ok = false;
      break;
    }
    uint8_t id[RECOVERY_OPERATION_ID_BYTES];
    for (size_t i = 0; i < sizeof id; ++i) {
      const char a = item->d_name[i * 2];
      const char b = item->d_name[i * 2 + 1];
      const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
      };
      const int hi = nibble(a), lo = nibble(b);
      if (hi < 0 || lo < 0) { ok = false; break; }
      id[i] = (uint8_t)((hi << 4) | lo);
    }
    if (!ok) break;

    char operation[PATH_CAP], source[PATH_CAP], intent[PATH_CAP];
    if (!operationPaths_(id, operation, sizeof operation,
                         source, sizeof source)) { ok = false; break; }
    struct stat opStat;
    if (stat(operation, &opStat) != 0 || !S_ISDIR(opStat.st_mode)) {
      ok = false;
      break;
    }
    const int n = snprintf(intent, sizeof intent, "%s/intent.bin", operation);
    if (n <= 0 || (size_t)n >= sizeof intent) { ok = false; break; }
    uint64_t intentSize = 0;
    bool intentExists = false;
    if (!regularFileSize(intent, intentSize, intentExists)) {
      ok = false;
      break;
    }
    if (!intentExists) {
      // Before intent publication, only empty directories and a partial
      // intent.tmp can exist. Any archive/phase artifact without intent means
      // the journal authority chain is incomplete; fail closed.
      if (orphanArtifactsWithoutIntent_(operation, source)) {
        ok = false;
        break;
      }
      continue;
    }

    if (!readIntent_(intent, plan) ||
        memcmp(plan.operationId, id, sizeof id) != 0) {
      ok = false;
      break;
    }
    bool archiveCommitted = false;
    bool complete = false;
    if (!archiveReady(plan, archiveCommitted) ||
        !completeMarker(plan, complete) ||
        !validateOperationContents_(plan, archiveCommitted, complete,
                                    archiveCommitted)) {
      ok = false;
      break;
    }
    if (complete) {
      // Completed archives are retained evidence, not scratch. The exact
      // operation/source directory inventories and every archive hash have
      // already been checked by validateOperationContents_().
      continue;
    }
    if (found) {
      ambiguous = true;
      ok = true;
      break;
    }
    found = true;
  }
  if (closedir(dir) != 0) ok = false;
  return ok;
}

}  // namespace att
