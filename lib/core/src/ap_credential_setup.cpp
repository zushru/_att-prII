#include "ap_credential_setup.h"
#include <string.h>

namespace att {
namespace {

// ConfigBootCoordinator accepts a gate only for the duration of its synchronous
// provision call. This stack adapter binds that call to the current request's
// authorization object; the coordinator never retains that object.
class RequestProvisionGate : public IConfigProvisionGate {
 public:
  RequestProvisionGate(const ApCredentialSetupCoordinator& setup,
                       const IConfigProvisionGate& requestAuthGate)
      : setup_(setup), requestAuthGate_(requestAuthGate) {}
  bool mayProvision() const override {
    return setup_.mayProvision(requestAuthGate_);
  }
 private:
  const ApCredentialSetupCoordinator& setup_;
  const IConfigProvisionGate& requestAuthGate_;
};

}  // namespace

ApCredentialSetupCoordinator::ApCredentialSetupCoordinator(
    ConfigBootCoordinator& boot, ApCredentialBootstrap& bootstrap,
    IApCredentialDisplay& display, ICredentialSoftAp& ap)
    : boot_(boot), bootstrap_(bootstrap), display_(display), ap_(ap),
      runningPassphrase_{0}, hasRunningPassphrase_(false), state_(ACSS_COLD) {}

ApCredentialSetupCoordinator::~ApCredentialSetupCoordinator() {
  clearRunningPassphrase_();
}

void ApCredentialSetupCoordinator::clearSecret_(void* value, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(value);
  while (length--) *p++ = 0;
}

bool ApCredentialSetupCoordinator::validDisclosureSecret_(const char* value,
                                                            size_t length) {
  if (value == nullptr ||
      (length != AP_CREDENTIAL_GENERATED_LENGTH &&
       length != AP_CREDENTIAL_PREVIOUS_HEX_LENGTH &&
       length != AP_CREDENTIAL_LEGACY_HEX_LENGTH)) return false;
  if (length == AP_CREDENTIAL_GENERATED_LENGTH) {
    for (size_t i = 0; i < length; ++i) {
      const char ch = value[i];
      const bool alphanumeric =
          (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9');
      if (!alphanumeric && ch != '-' && ch != '_') return false;
    }
  } else {
    for (size_t i = 0; i < length; ++i) {
      const char ch = value[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
        return false;
    }
  }
  return value[length] == '\0';
}

bool ApCredentialSetupCoordinator::equalSecret_(const char* left,
                                                  const char* right,
                                                  size_t length) {
  if (left == nullptr || right == nullptr) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i)
    difference |= static_cast<uint8_t>(left[i] ^ right[i]);
  return difference == 0;
}

void ApCredentialSetupCoordinator::clearRunningPassphrase_() {
  clearSecret_(runningPassphrase_, sizeof runningPassphrase_);
  hasRunningPassphrase_ = false;
}

bool ApCredentialSetupCoordinator::requestFaultClear_() {
  clearRunningPassphrase_();
  if (display_.requestBlankFrame()) {
    state_ = ACSS_FAULT;
    return true;
  }
  state_ = ACSS_STARTUP_CLEAR_PENDING;
  return false;
}

ApCredentialSetupCode ApCredentialSetupCoordinator::start() {
  if (state_ != ACSS_COLD) return ACSE_ALREADY_STARTED;
  if (!boot_.needsSetup()) return ACSE_BOOT_NOT_SETUP_REQUIRED;

  char passphrase[AP_CREDENTIAL_BUFFER_SIZE] = {0};
  const ApCredentialResult credential =
      bootstrap_.prepare(boot_.state(), nullptr, passphrase);
  if (credential.code != APCB_OK || !credential.disclose) {
    clearSecret_(passphrase, sizeof passphrase);
    state_ = ACSS_FAULT;
    return ACSE_BOOTSTRAP_FAILED;
  }

  size_t length = 0;
  while (length < sizeof passphrase && passphrase[length] != '\0') ++length;
  if (!validDisclosureSecret_(passphrase, length)) {
    clearSecret_(passphrase, sizeof passphrase);
    state_ = ACSS_FAULT;
    return ACSE_BOOTSTRAP_FAILED;
  }

  // The secured AP is deliberately not started until the credential frame has
  // been requested. Adafruit's API cannot confirm actual pixels; successful
  // DHCP assignment later provides end-to-end evidence that a client could use
  // the random secret that was disclosed through the OLED-only channel.
  if (!display_.requestCredentialDisplay(credential, passphrase, length)) {
    requestFaultClear_();
    clearSecret_(passphrase, sizeof passphrase);
    return ACSE_DISPLAY_FAILED;
  }

  if (!ap_.start(kProductionSoftApSsid, sizeof kProductionSoftApSsid - 1,
                 passphrase, length, kProductionSoftApChannel)) {
    requestFaultClear_();
    clearSecret_(passphrase, sizeof passphrase);
    return ACSE_AP_START_FAILED;
  }

  // Do not guess the gateway address. Read it from the live AP netif and put
  // the same credential and URL on the OLED before waiting for DHCP/code.
  char apAddress[16] = {0};
  if (!ap_.getAddress(apAddress, sizeof apAddress) ||
      !display_.requestNetworkInstructions(kProductionSoftApSsid, passphrase,
                                           length, apAddress)) {
    requestFaultClear_();
    clearSecret_(passphrase, sizeof passphrase);
    return ACSE_DISPLAY_FAILED;
  }

  // Retain the exact credential installed into the live AP so later NVS
  // read-back can detect a changed-but-well-formed value before provisioning.
  memcpy(runningPassphrase_, passphrase, sizeof runningPassphrase_);
  hasRunningPassphrase_ = true;
  clearSecret_(passphrase, sizeof passphrase);
  state_ = ACSS_WAITING_FOR_CLIENT;
  return ACSE_STARTED;
}

bool ApCredentialSetupCoordinator::mayProvision(
    const IConfigProvisionGate& requestAuthGate) const {
  return state_ == ACSS_WAITING_FOR_CLIENT && boot_.needsSetup() &&
         ap_.ready() && ap_.hasAssignedClientIp() &&
         requestAuthGate.mayProvision();
}

ApCredentialSetupValidationCode
ApCredentialSetupCoordinator::validateProvisionConfig(const Config& initial) {
  if (state_ != ACSS_WAITING_FOR_CLIENT || !boot_.needsSetup() ||
      !ap_.ready() || !ap_.hasAssignedClientIp())
    return ACSV_NOT_READY;

  char passphrase[AP_CREDENTIAL_BUFFER_SIZE] = {0};
  const ApCredentialResult credential =
      bootstrap_.loadExistingForSetup(passphrase);
  if (credential.code != APCB_OK) {
    clearSecret_(passphrase, sizeof passphrase);
    requestFaultClear_();
    return ACSV_CREDENTIAL_FAULT;
  }
  size_t passphraseLength = 0;
  while (passphraseLength < sizeof passphrase &&
         passphrase[passphraseLength] != '\0') ++passphraseLength;
  if (!hasRunningPassphrase_ ||
      !validDisclosureSecret_(passphrase, passphraseLength) ||
      !equalSecret_(passphrase, runningPassphrase_, sizeof runningPassphrase_)) {
    clearSecret_(passphrase, sizeof passphrase);
    requestFaultClear_();
    return ACSV_CREDENTIAL_FAULT;
  }

  // Config::wifiPass is replaced by the exact credential installed into AP.
  Config normalized = initial;
  memset(normalized.wifiPass, 0, sizeof normalized.wifiPass);
  memcpy(normalized.wifiPass, runningPassphrase_, sizeof normalized.wifiPass);
  const bool valid = validateConfigExceptAdminPin(normalized);
  clearSecret_(&normalized, sizeof normalized);
  clearSecret_(passphrase, sizeof passphrase);
  return valid ? ACSV_READY : ACSV_INVALID_CONFIG;
}

ApCredentialSetupCode ApCredentialSetupCoordinator::provision(
    const Config& initial, const IConfigProvisionGate& requestAuthGate,
    ConfigBootResult& bootResult) {
  if (state_ != ACSS_WAITING_FOR_CLIENT || !mayProvision(requestAuthGate))
    return ACSE_NOT_READY_FOR_PROVISION;

  // Re-read the durable credential and make it authoritative in the config
  // being committed. Otherwise a caller-supplied Config::wifiPass could differ
  // from the AP password just disclosed/used, causing the next READY boot to
  // fail the bootstrap mismatch check and strand the device.
  char passphrase[AP_CREDENTIAL_BUFFER_SIZE] = {0};
  // Never generate a replacement while the AP is already running.
  const ApCredentialResult credential =
      bootstrap_.loadExistingForSetup(passphrase);
  if (credential.code != APCB_OK) {
    clearSecret_(passphrase, sizeof passphrase);
    requestFaultClear_();
    return ACSE_BOOTSTRAP_FAILED;
  }
  size_t passphraseLength = 0;
  while (passphraseLength < sizeof passphrase &&
         passphrase[passphraseLength] != '\0') ++passphraseLength;
  if (!hasRunningPassphrase_ ||
      !validDisclosureSecret_(passphrase, passphraseLength) ||
      !equalSecret_(passphrase, runningPassphrase_, sizeof runningPassphrase_)) {
    clearSecret_(passphrase, sizeof passphrase);
    requestFaultClear_();
    return ACSE_BOOTSTRAP_FAILED;
  }

  Config provisionConfig = initial;
  memset(provisionConfig.wifiPass, 0, sizeof provisionConfig.wifiPass);
  memcpy(provisionConfig.wifiPass, runningPassphrase_,
         sizeof provisionConfig.wifiPass);
  clearSecret_(passphrase, sizeof passphrase);

  // Bind the core provisioner to this operation's request authorization. The
  // stack gate forwards the AP/DHCP/auth conditions and cannot outlive this call.
  RequestProvisionGate compositeGate(*this, requestAuthGate);
  bootResult = boot_.provision(provisionConfig, compositeGate);
  clearSecret_(&provisionConfig, sizeof provisionConfig);
  if (bootResult.code == CFG_BOOT_PROVISION_REJECTED) {
    // Authorization and config validation are checked before any durable
    // mutation. Keep the current AP credential/session alive so a fresh,
    // independently authorized request can correct and retry the wizard.
    return ACSE_CONFIG_REJECTED;
  }
  if (bootResult.code != CFG_BOOT_OK) {
    if (boot_.faulted()) {
      requestFaultClear_();
      return ACSE_PROVISION_FAULTED;
    }
    requestFaultClear_();
    return ACSE_CONFIG_REJECTED;
  }

  clearRunningPassphrase_();
  if (!display_.requestBlankFrame()) {
    state_ = ACSS_CLEAR_PENDING;
    return ACSE_PROVISIONED_CLEAR_PENDING;
  }
  state_ = ACSS_PROVISIONED;
  return ACSE_PROVISIONED_CLEAR_REQUESTED;
}

ApCredentialSetupCode ApCredentialSetupCoordinator::retryBlankFrame() {
  if (state_ == ACSS_STARTUP_CLEAR_PENDING) {
    if (!display_.requestBlankFrame()) return ACSE_STARTUP_CLEAR_PENDING;
    state_ = ACSS_FAULT;
    return ACSE_STARTUP_CLEAR_REQUESTED;
  }
  if (state_ != ACSS_CLEAR_PENDING || !boot_.ready())
    return ACSE_NOT_READY_FOR_PROVISION;
  if (!display_.requestBlankFrame()) return ACSE_PROVISIONED_CLEAR_PENDING;
  state_ = ACSS_PROVISIONED;
  return ACSE_PROVISIONED_CLEAR_REQUESTED;
}

}  // namespace att
