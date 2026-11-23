#pragma once

#include <stddef.h>
#include "ap_credential_bootstrap.h"

namespace att {

// Adapter port for disclosing and clearing a first-boot AP credential.
// A successful return means a transfer was requested, not that pixels were
// physically visible or acknowledged by the display controller.
class IApCredentialDisplay {
 public:
  virtual ~IApCredentialDisplay() {}
  virtual bool requestCredentialDisplay(const ApCredentialResult& result,
                                        const char* passphrase,
                                        size_t passphraseLength) = 0;
  virtual bool requestBlankFrame() = 0;
  // Shows the exact SSID, current AP password and AP URL after the AP starts.
  // Default false forces production compositions to opt in explicitly.
  virtual bool requestNetworkInstructions(const char* ssid,
                                          const char* passphrase,
                                          size_t passphraseLength,
                                          const char* ipv4Address) {
    (void)ssid; (void)passphrase; (void)passphraseLength;
    (void)ipv4Address; return false;
  }
};

}  // namespace att
