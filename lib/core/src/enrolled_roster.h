#pragma once
#include "roster_store.h"
namespace att {
class ISensorPageMap {
 public:
  virtual ~ISensorPageMap() {}
  virtual bool studentIdForPage(uint16_t physicalPage,
                                uint16_t& studentId) const = 0;
  // Reverse lookup is mandatory: assuming student ID == physical page + 1
  // here would silently bypass a later non-linear hardware mapping.
  virtual bool pageForStudentId(uint16_t studentId,
                                uint16_t& physicalPage) const = 0;
};

// Which student IDs currently have a fingerprint template in the sensor. Starts EMPTY AND INVALID: until a
// complete read of the sensor's index finishes, nobody counts as enrolled.
struct Occupancy {
  uint32_t w[10];
  bool valid;
  Occupancy() { clear(); }
  void clear() {
    for (int i = 0; i < 10; i++) w[i] = 0;
    valid = false;
    rejected_ = false;
    indexStarted_ = false;
    indexCapacity_ = 0;
    indexPageCount_ = 0;
    nextIndexPage_ = 0;
  }
  void begin() { clear(); }                       // start a refresh
  void finish() {
    valid = !rejected_ &&
        (!indexStarted_ || nextIndexPage_ == indexPageCount_);
  }
  void setPresent(uint16_t id, bool v);
  bool has(uint16_t id) const;
  size_t count() const;
  // AS608 ReadIndexTable page p (0..3): 32 bytes, bit j of byte i set =>
  // physical slot p*256+i*8+j holds a template [UNVERIFIED: bit order and
  // page size on the installed module]. Occupied slots are resolved to logical
  // student IDs using the injected map; slots >= capacity are ignored.
  // Decode physical sensor slots through the same mandatory page map used by
  // enrollment and attendance. A missing map invalidates the whole snapshot.
  void addIndexPage(uint8_t page, const uint8_t b[32], uint16_t capacity,
                    const ISensorPageMap* pageMap);
  static uint8_t pagesNeeded(uint16_t capacity) {
    return capacity < 1 || capacity > MAX_ID
        ? 0 : static_cast<uint8_t>((capacity + 255u) / 256u);
  }
 private:
  bool rejected_;
  bool indexStarted_;
  uint16_t indexCapacity_;
  uint8_t indexPageCount_;
  uint8_t nextIndexPage_;
};

// A student without a template can never scan, so counting them as expected would produce false Absent marks
// (after a restore onto a blank sensor, a lost template, a swapped sensor). They are excluded instead, which is
// visible (see reconcile) and fail-closed. An unreadable or unfinished sensor index excludes everybody.
class EnrolledRoster : public IRoster {
 public:
  EnrolledRoster(RosterStore& r, const Occupancy& o) : r_(r), o_(o) {}
  bool expected(uint16_t id, int32_t day) override {
    if (!historyComplete() || !o_.valid || !o_.has(id)) return false;
    const bool expected = r_.expected(id, day);
    return historyComplete() && expected;
  }
  // This reports roster-history integrity only. Invalid sensor occupancy yields
  // no expected IDs, but does not make the roster event history itself incomplete.
  bool historyComplete() const override { return r_.historyComplete(); }
 private:
  RosterStore& r_; const Occupancy& o_;
};

// ClosureEngine is constructed before AS608 startup because the sensor owner is
// wired through the attendance gate. During LogStore replay the index is not yet
// available, so preserve the roster semantics needed to validate historical
// CLOSE records. Runtime closure is not allowed until startup has produced a
// complete index; from that point forward only template-backed students count
// as expected. This also excludes roster entries added after the last enrollment.
class SensorAwareRoster : public IRoster {
 public:
  SensorAwareRoster(RosterStore& roster, const Occupancy& occupancy)
      : roster_(roster), occupancy_(occupancy) {}
  bool expected(uint16_t id, int32_t day) override {
    if (!historyComplete()) return false;
    if (occupancy_.valid && !occupancy_.has(id)) return false;
    const bool expected = roster_.expected(id, day);
    return historyComplete() && expected;
  }
  bool historyComplete() const override { return roster_.historyComplete(); }
 private:
  RosterStore& roster_;
  const Occupancy& occupancy_;
};

struct Reconcile {
  bool valid;                 // false: occupancy not trustworthy, all counts are 0
  uint16_t matched;           // active students with a template
  uint16_t orphans;           // active students WITHOUT a template: need (re-)enrolling, not expected until then
  uint16_t strays;            // templates whose ID is not a known student: never deleted automatically
  uint16_t firstOrphan, firstStray;   // 0 if none
};
Reconcile reconcile(const RosterStore& r, const Occupancy& o);
uint16_t nextToEnroll(const RosterStore& r, const Occupancy& o, uint16_t after);     // next active student lacking a template; 0 = none
uint16_t firstFreeSlot(const RosterStore& r, const Occupancy& o, uint16_t capacity);  // lowest ID neither known nor occupied; 0 = full
}
