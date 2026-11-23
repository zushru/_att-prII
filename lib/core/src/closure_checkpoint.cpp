#include "closure_checkpoint.h"
#include "event_codec.h"
#include <string.h>

namespace att {
namespace {
const uint8_t kMagic[4] = {'A', 'T', 'C', 'P'};

void put32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) |
      (static_cast<uint32_t>(in[1]) << 8) |
      (static_cast<uint32_t>(in[2]) << 16) |
      (static_cast<uint32_t>(in[3]) << 24);
}

void put16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

uint16_t get16(const uint8_t* in) {
  return static_cast<uint16_t>(in[0] |
      (static_cast<uint16_t>(in[1]) << 8));
}

bool supported(int32_t goLiveDay, uint32_t epoch, int16_t utcOffsetMin) {
  if (goLiveDay < 0 || goLiveDay > MAX_SUPPORTED_DAY ||
      utcOffsetMin < -720 || utcOffsetMin > 840 || epoch < EPOCH_2020)
    return false;
  const int32_t day = dayNumber(epoch, utcOffsetMin);
  return day >= goLiveDay && day <= MAX_SUPPORTED_DAY;
}
}  // namespace

bool encodeClosureCheckpoint(int32_t goLiveDay, uint32_t lastTickEpoch,
                             int16_t utcOffsetMin,
                             uint8_t out[CLOSURE_CHECKPOINT_SIZE]) {
  if (out == nullptr ||
      !supported(goLiveDay, lastTickEpoch, utcOffsetMin)) return false;

  uint8_t encoded[CLOSURE_CHECKPOINT_SIZE] = {};
  memcpy(encoded, kMagic, sizeof kMagic);
  encoded[4] = CLOSURE_CHECKPOINT_VERSION;
  // Bytes 5..7 are reserved and must remain zero in version 1.
  put32(encoded + 8, static_cast<uint32_t>(goLiveDay));
  put32(encoded + 12, lastTickEpoch);
  put16(encoded + 16, crc16(encoded, 16));
  memcpy(out, encoded, sizeof encoded);
  return true;
}

ClosureCheckpointDecodeCode decodeClosureCheckpoint(
    const uint8_t* bytes, size_t length, int32_t expectedGoLiveDay,
    int16_t utcOffsetMin, ClosureCheckpoint& out) {
  if (bytes == nullptr || length != CLOSURE_CHECKPOINT_SIZE)
    return CCP_BAD_ARGUMENT;
  if (memcmp(bytes, kMagic, sizeof kMagic) != 0) return CCP_BAD_MAGIC;
  if (bytes[4] != CLOSURE_CHECKPOINT_VERSION) return CCP_BAD_VERSION;
  if (bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0)
    return CCP_BAD_RESERVED;
  if (crc16(bytes, 16) != get16(bytes + 16)) return CCP_BAD_CRC;

  const uint32_t rawDay = get32(bytes + 8);
  const uint32_t epoch = get32(bytes + 12);
  if (rawDay > static_cast<uint32_t>(MAX_SUPPORTED_DAY) ||
      !supported(static_cast<int32_t>(rawDay), epoch, utcOffsetMin))
    return CCP_BAD_RANGE;
  if (static_cast<int32_t>(rawDay) != expectedGoLiveDay)
    return CCP_WRONG_GO_LIVE;

  const ClosureCheckpoint decoded = {
      static_cast<int32_t>(rawDay), epoch};
  out = decoded;
  return CCP_OK;
}

}  // namespace att
