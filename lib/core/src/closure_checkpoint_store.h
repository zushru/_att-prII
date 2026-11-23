#pragma once
#include "closure_checkpoint.h"

namespace att {

enum ClosureCheckpointStoreCode : uint8_t {
  CCS_OK,
  CCS_NOT_FOUND,
  CCS_ALREADY_EXISTS,
  CCS_NOT_READY,
  CCS_INVALID,
  CCS_TIME_ROLLBACK,
  CCS_IO_ERROR
};

// Platform-independent contract for the durable monotonic checkpoint.
// Implementations must read back every successful write and latch ambiguous
// results until a fresh boot/reconciliation.
class IClosureCheckpointStore {
 public:
  virtual ~IClosureCheckpointStore() {}
  virtual ClosureCheckpointStoreCode load(ClosureCheckpoint& out) = 0;
  virtual ClosureCheckpointStoreCode initialize(uint32_t nowEpoch) = 0;
  virtual ClosureCheckpointStoreCode advance(uint32_t nowEpoch) = 0;
};

}  // namespace att
