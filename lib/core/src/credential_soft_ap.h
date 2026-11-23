#pragma once

#include <stddef.h>

namespace att {

// Minimal AP port used by first-provision orchestration. The DHCP fact is
// historical for the current AP run; it is not identity or authentication.
class ICredentialSoftAp {
 public:
  virtual ~ICredentialSoftAp() {}
  virtual bool start(const char* ssid, size_t ssidLength,
                     const char* passphrase, size_t passphraseLength,
                     int channel) = 0;
  virtual bool ready() const = 0;
  virtual bool hasAssignedClientIp() const = 0;
  // Returns the AP interface's configured IPv4 address as dotted decimal.
  // This is required for first-run onboarding; callers must not guess it.
  virtual bool getAddress(char* output, size_t outputSize) const {
    (void)output; (void)outputSize; return false;
  }
};

}  // namespace att
