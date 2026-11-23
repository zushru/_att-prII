#include "enrolled_roster.h"
namespace att {
void Occupancy::setPresent(uint16_t id, bool v) {
  if (id < 1 || id > MAX_ID) return;
  if (v) w[id / 32] |= (1u << (id % 32)); else w[id / 32] &= ~(1u << (id % 32));
}
bool Occupancy::has(uint16_t id) const {
  if (id < 1 || id > MAX_ID) return false;
  return (w[id / 32] >> (id % 32)) & 1u;
}
size_t Occupancy::count() const {
  size_t n = 0;
  for (uint16_t id = 1; id <= MAX_ID; id++) if (has(id)) n++;
  return n;
}
void Occupancy::addIndexPage(uint8_t page, const uint8_t b[32], uint16_t capacity,
                             const ISensorPageMap* pageMap) {
  if (rejected_) return;
  const uint8_t requiredPages = pagesNeeded(capacity);
  if (!b || !pageMap || requiredPages == 0 || page >= requiredPages) {
    // An invalid page cannot be treated as an empty/valid sensor snapshot.
    for (int i = 0; i < 10; ++i) w[i] = 0;
    valid = false;
    rejected_ = true;
    return;
  }
  if (!indexStarted_) {
    indexStarted_ = true;
    indexCapacity_ = capacity;
    indexPageCount_ = requiredPages;
    nextIndexPage_ = 0;
  }
  if (capacity != indexCapacity_ || requiredPages != indexPageCount_ ||
      page != nextIndexPage_) {
    // A bitmap assembled from mixed, missing, duplicate, or reordered pages
    // cannot be used to classify a student as enrolled or absent.
    for (int i = 0; i < 10; ++i) w[i] = 0;
    valid = false;
    rejected_ = true;
    return;
  }
  for (int i = 0; i < 32; i++)
    for (int j = 0; j < 8; j++)
      if ((b[i] >> j) & 1u) {
        const uint32_t slot = (uint32_t)page * 256u +
            (uint32_t)i * 8u + (uint32_t)j;
        if (slot >= capacity) {
          // A set bit beyond the reported capacity means the index and system
          // parameters disagree. Ignoring it could hide an orphan template.
          for (int k = 0; k < 10; ++k) w[k] = 0;
          valid = false;
          rejected_ = true;
          return;
        }

        uint16_t studentId = 0;
        const bool mapped = pageMap->studentIdForPage(
            static_cast<uint16_t>(slot), studentId);
        // A bitmap slot with no valid logical identity, an out-of-range ID, or
        // two occupied pages resolving to the same student makes the *whole*
        // occupancy snapshot untrustworthy. Never publish a partial projection.
        if (!mapped || studentId < 1 || studentId > MAX_ID || has(studentId)) {
          for (int k = 0; k < 10; ++k) w[k] = 0;
          valid = false;
          rejected_ = true;
          return;
        }
        setPresent(studentId, true);
      }
  ++nextIndexPage_;
}
Reconcile reconcile(const RosterStore& r, const Occupancy& o) {
  Reconcile x = {o.valid && r.historyComplete(), 0, 0, 0, 0, 0};
  if (!x.valid) { return x; }
  for (uint16_t id = 1; id <= MAX_ID; id++) {
    bool occ = o.has(id);
    if (r.active(id)) {
      if (occ) x.matched++;
      else { if (!x.orphans) x.firstOrphan = id; x.orphans++; }
    } else if (occ && !r.known(id)) {
      if (!x.strays) x.firstStray = id;
      x.strays++;
    }
  }
  return x;
}
uint16_t nextToEnroll(const RosterStore& r, const Occupancy& o, uint16_t after) {
  if (!o.valid || !r.historyComplete()) return 0;
  for (uint16_t id = (uint16_t)(after + 1); id >= 1 && id <= MAX_ID; id++)
    if (r.active(id) && !o.has(id)) return id;
  return 0;
}
uint16_t firstFreeSlot(const RosterStore& r, const Occupancy& o, uint16_t capacity) {
  if (!o.valid || !r.historyComplete()) return 0;
  for (uint16_t id = 1; id <= capacity && id <= MAX_ID; id++)
    if (!r.known(id) && !o.has(id)) return id;       // IDs are never reused: known (even inactive) means taken
  return 0;
}
}
