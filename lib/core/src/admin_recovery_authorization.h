#pragma once

#include <stddef.h>
#include <stdint.h>

#include "admin_session.h"
#include "recovery_journal.h"

namespace att {

static const size_t RECOVERY_CONFIRMATION_MAX_BYTES = 61;

// Hardware integration implements this interface using a verified local
// service control. A caller-supplied HTTP boolean is not physical presence.
class IRecoveryPhysicalPresence {
 public:
  virtual ~IRecoveryPhysicalPresence() {}
  virtual bool asserted() const = 0;
};

// Canonical operator phrase: RECOVER <32 lowercase hex operation ID> <bytes>.
// The returned length excludes the trailing NUL.
bool formatRecoveryConfirmation(
    const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
    uint64_t retainedBytes, char* out, size_t capacity, size_t& outLength);

// Request-scoped adapter. It consumes its evidence on the first authorize()
// call and only refreshes the ADMIN token after the phrase and local-presence
// checks pass. The object must be constructed and used on the serialized
// application owner for one HTTP request only.
class AdminRecoveryAuthorization final : public IRecoveryAuthorization {
 public:
  AdminRecoveryAuthorization(
      AdminSessionBoundary& sessions, IRecoveryPhysicalPresence& presence,
      const uint8_t requestToken[16], uint32_t nowS,
      const char* typedConfirmation, size_t confirmationLength);
  ~AdminRecoveryAuthorization() override;

  AdminRecoveryAuthorization(const AdminRecoveryAuthorization&) = delete;
  AdminRecoveryAuthorization& operator=(
      const AdminRecoveryAuthorization&) = delete;

  bool authorize(const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
                 uint64_t retainedBytes) override;
  bool continueAuthorized() const override;
  bool usesPresence(const IRecoveryPhysicalPresence& presence) const {
    return &presence_ == &presence;
  }

 private:
  AdminSessionBoundary& sessions_;
  IRecoveryPhysicalPresence& presence_;
  uint8_t requestToken_[16];
  uint32_t nowS_;
  char typedConfirmation_[RECOVERY_CONFIRMATION_MAX_BYTES + 1];
  size_t confirmationLength_;
  bool inputValid_;
  bool consumed_;
  mutable bool authorized_;
};

}  // namespace att
