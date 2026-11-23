#include "admin_mutation_audit.h"
#include "attendance_mutation_request.h"

#include <stdio.h>

namespace att {
namespace {
bool isManualOperation(const Event& event) {
  return (event.type == EV_MARK || event.type == EV_TODAY_STATUS ||
          event.type == EV_NOCLASS_SET || event.type == EV_NOCLASS_CLEAR ||
          event.type == EV_AUTO_NOCLASS_REVIEW) &&
      !flagMore(event.flags);
}
bool validRequestMarker(const Event& marker) {
  return marker.type == EV_ADMIN_MUTATION && marker.flags == FL_MORE &&
      (marker.a != 0 || marker.b != 0 || marker.epoch != 0);
}
bool validAutomaticReviewOperation(const Event& event) {
  return event.type == EV_AUTO_NOCLASS_REVIEW &&
      (event.flags == 0 || event.flags == FL_SESSION) &&
      (event.a == ANCR_CONFIRM || event.a == ANCR_UNDO) &&
      event.epoch != 0 && event.b <= MAX_SUPPORTED_DAY;
}
}  // namespace

bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit,
    const Event* todayCas0, const Event* todayCas1, const Event* todayCas2,
    const Event& operation, AdminMutationAuditJsonFields& out) {
  out = {};
  const char nullValue[] = "null";
  for (size_t i = 0; i < sizeof nullValue; ++i) {
    out.requestId[i] = nullValue[i];
    out.reason[i] = nullValue[i];
    out.adminSession[i] = nullValue[i];
  }
  if (!isManualOperation(operation)) return false;
  if (marker == nullptr)
    return operation.type != EV_TODAY_STATUS &&
        operation.type != EV_AUTO_NOCLASS_REVIEW && audit == nullptr &&
        todayCas0 == nullptr && todayCas1 == nullptr && todayCas2 == nullptr;
  if (!validRequestMarker(*marker) || marker->seq == UINT32_MAX)
    return false;

  if (audit == nullptr) {
    if (todayCas0 != nullptr || todayCas1 != nullptr || todayCas2 != nullptr ||
        marker->seq + 1u != operation.seq) return false;
  } else {
    if (marker->seq + 1u != audit->seq || audit->seq == UINT32_MAX ||
        audit->type != EV_ADMIN_AUDIT || audit->flags != FL_MORE)
      return false;
    const bool reasonlessAutoNoClassReview = audit->a == AMR_NONE &&
        operation.type == EV_AUTO_NOCLASS_REVIEW;
    const bool reasonlessToday = audit->a == AMR_NONE &&
        operation.type == EV_TODAY_STATUS;
    if (reasonlessAutoNoClassReview) {
      if (todayCas0 == nullptr || todayCas1 == nullptr || todayCas2 == nullptr ||
          audit->seq > UINT32_MAX - 5u ||
          audit->seq + 1u != todayCas0->seq ||
          todayCas0->type != EV_TODAY_CAS ||
          (todayCas0->flags != FL_MORE &&
           todayCas0->flags != (FL_MORE | FL_SESSION)) ||
          todayCas0->a != AUTO_NOCLASS_REVIEW_CAS_MAGIC ||
          todayCas0->epoch == 0 || todayCas0->epoch >= marker->seq ||
          todayCas0->b != operation.b ||
          todayCas0->seq + 1u != todayCas1->seq ||
          todayCas1->type != EV_TODAY_CAS ||
          todayCas1->flags != (FL_MORE | 0x02u) ||
          todayCas1->seq + 1u != todayCas2->seq ||
          todayCas2->type != EV_TODAY_CAS ||
          todayCas2->flags != (FL_MORE | 0x04u) ||
          todayCas2->a != 0 || todayCas2->b != 0 ||
          todayCas2->seq + 1u != operation.seq ||
          !validAutomaticReviewOperation(operation) ||
          flagSession(todayCas0->flags) != flagSession(operation.flags))
        return false;
      const uint32_t expectedRecords = static_cast<uint32_t>(todayCas1->a) |
          (static_cast<uint32_t>(todayCas1->b) << 16);
      const uint64_t generation =
          (static_cast<uint64_t>(todayCas1->epoch) << 32) | todayCas2->epoch;
      if (todayCas0->epoch > expectedRecords ||
          expectedRecords > ATTENDANCE_MUTATION_MAX_PRE_APPEND_RECORDS ||
          generation == 0) return false;
    } else if (reasonlessToday) {
      if (todayCas0 == nullptr || todayCas1 == nullptr || todayCas2 == nullptr ||
          audit->seq > UINT32_MAX - 4u ||
          audit->seq + 1u != todayCas0->seq ||
          todayCas0->type != EV_TODAY_CAS || todayCas0->flags != FL_MORE ||
          todayCas0->seq + 1u != todayCas1->seq ||
          todayCas1->type != EV_TODAY_CAS ||
          todayCas1->flags != (FL_MORE | 0x02u) ||
          todayCas1->seq + 1u != todayCas2->seq ||
          todayCas2->type != EV_TODAY_CAS ||
          todayCas2->flags != (FL_MORE | 0x04u) ||
          todayCas2->seq + 1u != operation.seq)
        return false;
      const uint8_t status = static_cast<uint8_t>(
          (todayCas0->epoch >> 24) & 0xFFu);
      const uint16_t day = static_cast<uint16_t>(todayCas0->epoch & 0xFFFFu);
      const uint8_t session = static_cast<uint8_t>(
          (todayCas0->epoch >> 16) & 0xFFu);
      const bool statusMatches = status == ST_PRESENT || status == ST_PENDING ||
          status == ST_EXCUSED;
      const uint8_t committedStatus = flagStatus(operation.flags);
      const bool committedStatusValid = committedStatus == ST_PRESENT ||
          committedStatus == ST_LATE || committedStatus == ST_PENDING ||
          committedStatus == ST_EXCUSED;
      // The stored request records the operator's intent. For the Present
      // choice, the firmware may commit Late when its trusted RTC is beyond
      // the configured threshold. Audit/history must validate that one
      // deliberate derivation rather than require intent == stored outcome.
      const bool committedStatusMatches = status == ST_PRESENT
          ? (committedStatus == ST_PRESENT || committedStatus == ST_LATE)
          : committedStatus == status;
      const bool matches = todayCas0->a == operation.a &&
          todayCas0->b == 0 && day == operation.b && session <= 1 &&
          statusMatches && committedStatusValid && committedStatusMatches &&
          flagSession(operation.flags) == session;
      const bool generationNonzero = todayCas2->a != 0 ||
          todayCas2->epoch != 0 || todayCas2->b != 0;
      if (!matches || !generationNonzero) return false;
    } else if (todayCas0 != nullptr || todayCas1 != nullptr ||
               todayCas2 != nullptr || audit->seq + 1u != operation.seq) {
      return false;
    }
    const uint64_t fingerprint = static_cast<uint64_t>(audit->epoch) |
        (static_cast<uint64_t>(audit->b) << 32);
    if (fingerprint == 0 ||
        (audit->a == AMR_NONE && operation.type != EV_TODAY_STATUS &&
         operation.type != EV_AUTO_NOCLASS_REVIEW) ||
        (audit->a != AMR_NONE &&
         (audit->a < AMR_DATA_ENTRY_ERROR || audit->a > AMR_OTHER_DOCUMENTED)))
      return false;
  }

  const int requestLength = snprintf(out.requestId, sizeof out.requestId,
      "\"%04x%04x%04x%04x\"", static_cast<unsigned>(marker->a),
      static_cast<unsigned>(marker->b),
      static_cast<unsigned>(marker->epoch >> 16),
      static_cast<unsigned>(marker->epoch & 0xFFFFu));
  if (requestLength != 18) return false;

  if (audit != nullptr) {
    const AdminMutationReason reason =
        static_cast<AdminMutationReason>(audit->a);
    const char* reasonName = adminMutationReasonName(reason);
    int reasonLength = 4;  // literal null for the intentionally reasonless Today action
    if (reasonName != nullptr) {
      reasonLength = snprintf(out.reason, sizeof out.reason,
                              "\"%s\"", reasonName);
    } else if (operation.type != EV_TODAY_STATUS &&
               operation.type != EV_AUTO_NOCLASS_REVIEW) {
      return false;
    }
    const int sessionLength = snprintf(out.adminSession,
        sizeof out.adminSession, "\"%04x%08lx\"",
        static_cast<unsigned>(audit->b),
        static_cast<unsigned long>(audit->epoch));
    if (reasonLength <= 0 ||
        static_cast<size_t>(reasonLength) >= sizeof out.reason ||
        sessionLength != 14) return false;
  }
  return true;
}

bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit, const Event* todayCas,
    const Event& operation, AdminMutationAuditJsonFields& out) {
  // Old callers may still inspect legacy three-record groups. A single-CAS
  // shape is no longer a valid Today mutation, so reject it rather than
  // weakening the new six-record event contract.
  return formatAdminMutationAuditJsonFields(marker, audit, todayCas, nullptr,
      nullptr, operation, out);
}

bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit, const Event& operation,
    AdminMutationAuditJsonFields& out) {
  return formatAdminMutationAuditJsonFields(marker, audit, nullptr, nullptr,
      nullptr, operation, out);
}

}  // namespace att
