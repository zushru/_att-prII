#if defined(ESP_PLATFORM)

#include "esp32_recovery_presence.h"

namespace att {

Esp32GpioRecoveryPresence::Esp32GpioRecoveryPresence(
    int gpioPin, bool activeHigh)
    : gpioPin_(gpioPin), activeLevel_(activeHigh ? 1 : 0), ready_(false) {}

bool Esp32GpioRecoveryPresence::reservedPin_(int gpioPin) {
  // ESP32 flash, boot-strapping and console pins are not accepted as a
  // maintenance input. Also exclude pins assigned to I2C, AS608 UART and
  // buzzer. GPIO4 is intentionally available for the AS608 TCH output.
  if (gpioPin >= 6 && gpioPin <= 11) return true;
  // Classic ESP32 GPIO34-39 are input-only and have no internal pulls. This
  // adapter relies on an internal inactive bias, so accepting them would
  // violate its fail-inactive contract.
  if (gpioPin >= 34 && gpioPin <= 39) return true;
  switch (gpioPin) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 5:
    case 12:
    case 15:
    case 16:
    case 17:
    case 21:
    case 22:
    case 23:
      return true;
    default:
      return false;
  }
}

bool Esp32GpioRecoveryPresence::begin() {
  ready_ = false;
  if (!GPIO_IS_VALID_GPIO(gpioPin_) || reservedPin_(gpioPin_))
    return false;

  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << static_cast<uint32_t>(gpioPin_);
  config.mode = GPIO_MODE_INPUT;
  // Bias toward the inactive state. For the AS608 TCH active-high signal this
  // means a pull-down, so an unplugged/floating line cannot look asserted.
  config.pull_up_en = activeLevel_ == 0 ? GPIO_PULLUP_ENABLE
                                        : GPIO_PULLUP_DISABLE;
  config.pull_down_en = activeLevel_ == 1 ? GPIO_PULLDOWN_ENABLE
                                          : GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) return false;

  ready_ = true;
  return true;
}

bool Esp32GpioRecoveryPresence::asserted() const {
  if (!ready_) return false;
  return gpio_get_level(static_cast<gpio_num_t>(gpioPin_)) == activeLevel_;
}

}  // namespace att

#endif  // ESP_PLATFORM
