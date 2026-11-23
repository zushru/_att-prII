#pragma once
#include "event_codec.h"
#include "istorage.h"
namespace att {
const size_t MAX_GROUP = 16;
const size_t MAX_OBSERVERS = 16;
struct OpenInfo {
  bool ok;
  uint32_t records, lastSeq, baselineEpoch;
  uint32_t tornBytes;
  uint32_t droppedGroup;
  int32_t corruptAt;
  uint64_t generation;   // identifies the specific LogStore::open() result
  // Diagnostic only: true when the complete structural first-pass scan and
  // trailing-group analysis finished. These fields do not authorize repair.
  bool structuralScanComplete;
  uint32_t structurallyValidRecords;       // sequential, decodable prefix
  uint32_t completeGroupPrefixRecords;     // excludes a trailing FL_MORE group
};
// Derived state (indexes, caches) follows the log. onReset = log (re)opened: discard and replay.
class LogObserver {
 public:
  virtual ~LogObserver() {}
  virtual void onReset() = 0;
  virtual void onRecord(const Event& e) = 0;   // in log order, only after the record is durable
  virtual void onAttached() {}                // successful registration after a complete replay
  virtual bool canAttach() const { return true; }  // single-owner projections may reject another LogStore
  // False means the observer can no longer represent the complete log prefix.
  // The store then latches faulted and requires a successful reopen/replay.
  virtual bool healthy() const { return true; }
};
class LogStore {
 public:
  explicit LogStore(IStorage& s, uint32_t firstSeq = 1)
      : s_(s), count_(0), nextSeq_(firstSeq), firstSeq_(firstSeq), baseline_(0),
        openGeneration_(0), faulted_(true), nobs_(0) {
    for (size_t i = 0; i < MAX_OBSERVERS; i++) obs_[i] = nullptr;
  }
  OpenInfo open();
  // false latches faulted(); storage may contain none, partial, or all bytes,
  // or a committed record may have been rejected by a projection observer.
  // Reopen/reconcile before retrying the same logical operation.
  bool append(const Event* evs, size_t n);
  bool at(size_t i, Event& e);
  // Replays the current prefix to one observer.
  bool attach(LogObserver* o);
  // Atomically registers a related observer set. Before the first open(),
  // registration is deferred and the first successful open() replays the
  // validated prefix before onAttached(). On an open healthy log, attachment
  // replays the current prefix immediately. Either the whole set attaches or none.
  bool attachMany(LogObserver* const* observers, size_t count);
  bool hasObserver(const LogObserver* observer) const;
  bool usesStorage(const IStorage& storage) const { return &s_ == &storage; }
  // Storage-level contract: successful append already includes exact-byte readback.
  bool appendVerifiesExactBytes() const { return s_.appendVerifiesExactBytes(); }
  size_t count() const { return count_; }
  uint32_t lastSeq() const { return nextSeq_ == 0 ? 0 : nextSeq_ - 1; }
  uint32_t baselineEpoch() const { return baseline_; }
  uint64_t openGeneration() const { return openGeneration_; }
  bool faulted() const { return faulted_; }
  // External postcondition checks may discover an ambiguous durable result.
  // Fence every writer until reopen/replay; this never repairs or truncates.
  void requireReopen() { faulted_ = true; }
  // Preflight a durable group. Latches faulted if any attached projection is unhealthy.
  bool readyToAppend();
 private:
  void track(const Event& e);
  bool observersHealthy() const;
  void notifyRecord(const Event& e);
  void notifyReset();
  IStorage& s_;
  size_t count_;
  uint32_t nextSeq_, firstSeq_, baseline_;
  uint64_t openGeneration_;
  bool faulted_;
  LogObserver* obs_[MAX_OBSERVERS];
  size_t nobs_;
};
// Chunks of GAP records that fit in maxOut events. *next (optional) = where coverage stopped; == to when complete.
// A caller that ignores *next silently drops the tail of a long span: use appendGaps() or check it.
// True only when whole-minute GAP records can cover [from,to) without the rounded
// exclusive endpoint exceeding UINT32_MAX. Empty spans are representable; reversed
// spans are not.
bool gapSpanEncodable(uint32_t from, uint32_t to);
size_t makeGapEvents(uint32_t from, uint32_t to, uint16_t cause, Event* out, size_t maxOut, uint32_t* next = nullptr);
// Records GAPs covering [from,to) in as many commit groups as needed. Invalid or
// unrepresentable ranges are rejected before writing. False after preflight means an
// append was attempted and may have persisted none, part, or all of its group.
// Repeating after reopen can append duplicate GAP records: this is not exactly-once.
bool appendGaps(LogStore& log, uint32_t from, uint32_t to, uint16_t cause);
}
