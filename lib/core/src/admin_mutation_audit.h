#pragma once

#include "event_codec.h"

namespace att {

struct AdminMutationAuditJsonFields {
  // Complete JSON string values, including quotes, or the literal "null".
  char requestId[19];
  char reason[32];
  char adminSession[15];
};

// Derive optional history-response fields from the records preceding an
// attendance operation. Supports legacy events, historical three-record
// groups, and the six-record reasonless Today status group. Invalid/incomplete
// metadata returns false; callers must fail closed.
bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit,
    const Event* todayCas0, const Event* todayCas1, const Event* todayCas2,
    const Event& operation, AdminMutationAuditJsonFields& out);
// Compatibility overload for the former single-CAS experimental format.
bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit, const Event* todayCas,
    const Event& operation, AdminMutationAuditJsonFields& out);
bool formatAdminMutationAuditJsonFields(
    const Event* marker, const Event* audit, const Event& operation,
    AdminMutationAuditJsonFields& out);

}  // namespace att
