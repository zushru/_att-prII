#pragma once
#include <stddef.h>
#include <stdint.h>
namespace att {
enum EvType : uint8_t {
  EV_NONE = 0, EV_SCAN, EV_MARK, EV_NOCLASS_SET, EV_NOCLASS_CLEAR, EV_CLOSE,
  EV_GAP, EV_CLOCK_SET, EV_ROSTER, EV_SETTING, EV_PAYLOAD, EV_BOOT, EV_REPAIR,
  EV_ADMIN_MUTATION, EV_ADMIN_AUDIT, EV_TODAY_CAS, EV_TODAY_STATUS,
  EV_AUTO_NOCLASS_SET, EV_AUTO_NOCLASS_REVIEW,
  EV_MAX = EV_AUTO_NOCLASS_REVIEW
};
// Required reason categories for new authenticated manual corrections. NONE is
// accepted only when replaying legacy requests that predate audit metadata.
enum AdminMutationReason : uint16_t {
  AMR_NONE = 0,
  AMR_DATA_ENTRY_ERROR = 1,
  AMR_SOURCE_RECORD = 2,
  AMR_STAFF_CONFIRMATION = 3,
  AMR_ADMINISTRATIVE_APPROVAL = 4,
  AMR_OTHER_DOCUMENTED = 5
};
enum GapCause : uint16_t { GAP_POWER = 1, GAP_RTC = 2, GAP_STORAGE = 3, GAP_SENSOR = 4, GAP_CLOCK = 5 };
enum ClockSrc : uint16_t { SRC_MANUAL = 0, SRC_AUTO = 1, SRC_WIZARD = 2 };
enum StatusCode : uint8_t { ST_CLEAR = 0, ST_PRESENT = 1, ST_LATE = 2, ST_EXCUSED = 3, ST_PENDING = 4 };
enum AutoNoClassReviewAction : uint16_t { ANCR_CONFIRM = 1, ANCR_UNDO = 2 };
const uint16_t AUTO_NOCLASS_REVIEW_CAS_MAGIC = 0xA11Cu;
const uint8_t FL_SESSION = 0x01;   // 0 = AM, 1 = PM
const uint8_t FL_COVERAGE = 0x08;
const uint8_t FL_MORE = 0x10;      // another record of this commit group follows
const uint8_t FL_WHOLEDAY = 0x20;
// EV_TODAY_STATUS-only extension: status code 4 (Pending). Reserved for all
// legacy event types, whose validators continue rejecting this bit.
const uint8_t FL_STATUS_EXT = 0x40;
const size_t REC_SIZE = 16;

struct Event { uint8_t type; uint8_t flags; uint16_t a; uint32_t epoch; uint32_t seq; uint16_t b; };

uint16_t crc16(const uint8_t* d, size_t n);                 // CRC-16/CCITT-FALSE
void encode(const Event& e, uint8_t out[REC_SIZE]);         // little-endian, CRC in bytes 14..15
bool decode(const uint8_t in[REC_SIZE], Event& e);          // false: bad CRC or invalid type; e untouched

uint8_t makeFlags(uint8_t session, uint8_t status, bool coverage, bool more, bool wholeDay);
uint8_t makeTodayStatusFlags(uint8_t session, uint8_t status);
uint8_t flagSession(uint8_t f);
uint8_t flagStatus(uint8_t f);
bool flagCoverage(uint8_t f);
bool flagMore(uint8_t f);
bool flagWholeDay(uint8_t f);

Event makePayload(uint64_t bits, uint32_t seq, bool more);  // 64 raw bits in a, epoch, b
uint64_t payloadBits(const Event& e);
uint16_t payloadCount(uint16_t cap);                        // payload records for IDs 1..cap
void bitmapSet(uint64_t* words, uint16_t id);               // id is 1-based
bool bitmapTest(const uint64_t* words, uint16_t id);
}
