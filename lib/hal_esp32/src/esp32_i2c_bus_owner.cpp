#include "esp32_i2c_bus_owner.h"

namespace att {
namespace {
const uint16_t kI2cTransactionTimeoutMs = 50;
}


bool Esp32I2cBusOwner::acquire() {
  // Read readiness only after owning the mutex. Otherwise a client could
  // observe ready_=true, wait behind recover(), then enter the bus after a
  // failed restart has left ready_=false.
  if (!lock_.acquire()) return false;
  if (!ready_) {
    lock_.release();
    return false;
  }
  return true;
}

void Esp32I2cBusOwner::release() {
  lock_.release();
}

bool Esp32I2cBusOwner::begin(int sda, int scl, uint32_t clockHz) {
  if (ready_) return true;
  if (attempted_ || sda < 0 || scl < 0 || sda == scl || clockHz == 0 ||
      !lock_.valid()) return false;

  attempted_ = true;
  sda_ = sda;
  scl_ = scl;
  clockHz_ = clockHz;
  // Keep a missing/unplugged peripheral from blocking the application task.
  // Arduino-ESP32 defaults this to 50 ms; set it explicitly as a board policy.
  wire_.setTimeOut(kI2cTransactionTimeoutMs);
  ready_ = wire_.begin(sda_, scl_, clockHz_);
  initializedOnce_ = ready_;
  return ready_;
}

bool Esp32I2cBusOwner::recover() {
  // Only recover a bus that completed its first initialization. A failed
  // initial begin has unknown partial state and remains terminal.
  if (!attempted_ || !initializedOnce_ || !lock_.valid()) return false;
  if (!lock_.acquire()) return false;

  // Block new client transactions while the underlying TwoWire instance is
  // deinitialized/reinitialized. Do not use release() here: ready_ is false
  // during the restart, but the raw mutex must still be released.
  ready_ = false;
  const bool ended = wire_.end();
  if (ended) {
    wire_.setTimeOut(kI2cTransactionTimeoutMs);
    ready_ = wire_.begin(sda_, scl_, clockHz_);
  }
  const bool recovered = ready_;
  if (!recovered) initializedOnce_ = false;
  lock_.release();
  return recovered;
}

}  // namespace att
