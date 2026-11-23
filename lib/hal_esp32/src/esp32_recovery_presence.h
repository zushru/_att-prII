#pragma once

#if defined(ESP_PLATFORM)

#include "admin_recovery_authorization.h"

#include <driver/gpio.h>

namespace att {

// Configurable local service-presence input. There is deliberately no default
// pin: board integration must supply the verified input and active polarity.
// The internal pull biases the line toward its inactive state so a disconnected
// input fails closed; the connected source must still provide valid logic levels.
class Esp32GpioRecoveryPresence final : public IRecoveryPhysicalPresence {
 public:
  Esp32GpioRecoveryPresence(int gpioPin, bool activeHigh);

  Esp32GpioRecoveryPresence(const Esp32GpioRecoveryPresence&) = delete;
  Esp32GpioRecoveryPresence& operator=(
      const Esp32GpioRecoveryPresence&) = delete;

  bool begin();
  bool ready() const { return ready_; }
  bool asserted() const override;
  int gpioPin() const { return gpioPin_; }

 private:
  static bool reservedPin_(int gpioPin);

  int gpioPin_;
  int activeLevel_;
  bool ready_;
};

}  // namespace att

#endif  // ESP_PLATFORM
