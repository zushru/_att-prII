#include "admin_recovery_authorization.h"

#include <string.h>

namespace att {
namespace {

const char kRecoveryPrefix[] = "RECOVER ";
const char kHexDigits[] = "0123456789abcdef";

void secureClear(void* value, size_t length) {
  volatile uint8_t* cursor = static_cast<volatile uint8_t*>(value);
  while (length-- > 0) *cursor++ = 0;
}

bool constantTimePhraseEqual(
    const char* expected, size_t expectedLength,
    const char* supplied, size_t suppliedLength) {
  // Both arrays have the same fixed capacity and are zero-padded. Compare
  // every byte regardless of the first mismatch or the supplied length.
  uint32_t difference =
      static_cast<uint32_t>(expectedLength ^ suppliedLength);
  for (size_t i = 0; i <= RECOVERY_CONFIRMATION_MAX_BYTES; ++i) {
    difference |= static_cast<uint8_t>(expected[i]) ^
                  static_cast<uint8_t>(supplied[i]);
  }
  return difference == 0;
}

}  // namespace

bool formatRecoveryConfirmation(
    const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
    uint64_t retainedBytes, char* out, size_t capacity, size_t& outLength) {
  outLength = 0;
  if (out != nullptr && capacity > 0) out[0] = '\0';
  if (operationId == nullptr || out == nullptr || capacity == 0)
    return false;
  uint8_t operationIdBits = 0;
  for (size_t i = 0; i < RECOVERY_OPERATION_ID_BYTES; ++i)
    operationIdBits |= operationId[i];
  if (operationIdBits == 0) return false;

  char decimal[20];
  size_t decimalLength = 0;
  do {
    decimal[decimalLength++] =
        static_cast<char>('0' + (retainedBytes % 10u));
    retainedBytes /= 10u;
  } while (retainedBytes != 0 && decimalLength < sizeof decimal);
  if (retainedBytes != 0) return false;

  const size_t required = (sizeof kRecoveryPrefix - 1u) +
      RECOVERY_OPERATION_ID_BYTES * 2u + 1u + decimalLength;
  if (required > RECOVERY_CONFIRMATION_MAX_BYTES ||
      capacity <= required)
    return false;

  size_t position = 0;
  memcpy(out + position, kRecoveryPrefix, sizeof kRecoveryPrefix - 1u);
  position += sizeof kRecoveryPrefix - 1u;

  for (size_t i = 0; i < RECOVERY_OPERATION_ID_BYTES; ++i) {
    const uint8_t value = operationId[i];
    out[position++] = kHexDigits[value >> 4];
    out[position++] = kHexDigits[value & 0x0fu];
  }

  out[position++] = ' ';
  while (decimalLength > 0)
    out[position++] = decimal[--decimalLength];
  out[position] = '\0';
  outLength = position;
  secureClear(decimal, sizeof decimal);
  return true;
}

AdminRecoveryAuthorization::AdminRecoveryAuthorization(
    AdminSessionBoundary& sessions, IRecoveryPhysicalPresence& presence,
    const uint8_t requestToken[16], uint32_t nowS,
    const char* typedConfirmation, size_t confirmationLength)
    : sessions_(sessions),
      presence_(presence),
      requestToken_{0},
      nowS_(nowS),
      typedConfirmation_{0},
      confirmationLength_(0),
      inputValid_(false),
      consumed_(false),
      authorized_(false) {
  if (requestToken == nullptr || typedConfirmation == nullptr ||
      confirmationLength == 0 ||
      confirmationLength > RECOVERY_CONFIRMATION_MAX_BYTES)
    return;

  memcpy(requestToken_, requestToken, sizeof requestToken_);
  memcpy(typedConfirmation_, typedConfirmation, confirmationLength);
  confirmationLength_ = confirmationLength;
  inputValid_ = true;
}

AdminRecoveryAuthorization::~AdminRecoveryAuthorization() {
  secureClear(requestToken_, sizeof requestToken_);
  secureClear(typedConfirmation_, sizeof typedConfirmation_);
  confirmationLength_ = 0;
  inputValid_ = false;
  consumed_ = true;
  authorized_ = false;
}

bool AdminRecoveryAuthorization::authorize(
    const uint8_t operationId[RECOVERY_OPERATION_ID_BYTES],
    uint64_t retainedBytes) {
  if (consumed_) return false;
  consumed_ = true;
  authorized_ = false;

  char expected[RECOVERY_CONFIRMATION_MAX_BYTES + 1] = {};
  size_t expectedLength = 0;
  const bool expectedReady = formatRecoveryConfirmation(
      operationId, retainedBytes, expected, sizeof expected, expectedLength);

  // The ADMIN check is deliberately non-refreshing. Wrong/expired sessions,
  // mismatched phrases and absent local presence must not extend idle expiry.
  const bool sessionMatches = inputValid_ &&
      sessions_.recoverySessionMatches_(requestToken_, nowS_);
  const bool phraseMatches = inputValid_ && expectedReady &&
      constantTimePhraseEqual(
          expected, expectedLength, typedConfirmation_,
          confirmationLength_);
  const bool localPresence = sessionMatches && phraseMatches &&
      presence_.asserted();

  bool accepted = false;
  if (localPresence) {
    const ApiResult refreshed =
        sessions_.authorizeRecoverySession_(requestToken_, nowS_);
    accepted = refreshed.code == A_OK;
  }

  secureClear(expected, sizeof expected);
  secureClear(requestToken_, sizeof requestToken_);
  secureClear(typedConfirmation_, sizeof typedConfirmation_);
  confirmationLength_ = 0;
  inputValid_ = false;
  authorized_ = accepted;
  return accepted;
}

bool AdminRecoveryAuthorization::continueAuthorized() const {
  if (!authorized_) return false;
  if (!presence_.asserted()) {
    authorized_ = false;
    return false;
  }
  return true;
}

}  // namespace att
