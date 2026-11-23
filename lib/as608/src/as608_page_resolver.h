#pragma once
#include <stdint.h>
#include "as608_attendance_pipeline.h"

namespace att {

// One-to-one mapping over explicitly configured, contiguous page and student
// ranges. The current firmware injects a provisional page-0 -> student-1 policy.
// Keep all physical/logical translation here so a verified sensor-specific policy
// can replace it without edits to enrollment, index decoding or scan admission.
class LinearAs608PageResolver final : public IAs608PageResolver {
 public:
  LinearAs608PageResolver(uint16_t firstPhysicalPage,
                          uint16_t firstStudentId,
                          uint16_t mappingCount);

  bool valid() const { return valid_; }
  bool studentIdForPage(uint16_t physicalPage,
                        uint16_t& studentId) const override;
  bool pageForStudentId(uint16_t studentId,
                        uint16_t& physicalPage) const override;

 private:
  uint16_t firstPhysicalPage_;
  uint16_t firstStudentId_;
  uint16_t mappingCount_;
  bool valid_;
};

}  // namespace att
