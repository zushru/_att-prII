#include "as608_page_resolver.h"

namespace att {

LinearAs608PageResolver::LinearAs608PageResolver(
    uint16_t firstPhysicalPage, uint16_t firstStudentId,
    uint16_t mappingCount)
    : firstPhysicalPage_(firstPhysicalPage),
      firstStudentId_(firstStudentId),
      mappingCount_(mappingCount),
      valid_(false) {
  if (mappingCount_ == 0 ||
      firstStudentId_ == 0 ||
      firstStudentId_ > MAX_ID) return;

  const uint32_t studentCountAvailable =
      static_cast<uint32_t>(MAX_ID) - firstStudentId_ + 1u;
  const uint32_t physicalLast =
      static_cast<uint32_t>(firstPhysicalPage_) +
      static_cast<uint32_t>(mappingCount_) - 1u;
  valid_ = static_cast<uint32_t>(mappingCount_) <= studentCountAvailable &&
      physicalLast <= UINT16_MAX;
}

bool LinearAs608PageResolver::studentIdForPage(
    uint16_t physicalPage, uint16_t& studentId) const {
  if (!valid_ || physicalPage < firstPhysicalPage_) return false;

  const uint32_t offset =
      static_cast<uint32_t>(physicalPage) - firstPhysicalPage_;
  if (offset >= mappingCount_) return false;

  const uint32_t resolved =
      static_cast<uint32_t>(firstStudentId_) + offset;
  if (resolved == 0 || resolved > MAX_ID) return false;

  studentId = static_cast<uint16_t>(resolved);
  return true;
}

bool LinearAs608PageResolver::pageForStudentId(
    uint16_t studentId, uint16_t& physicalPage) const {
  if (!valid_ || studentId < firstStudentId_) return false;
  const uint32_t offset =
      static_cast<uint32_t>(studentId) - firstStudentId_;
  if (offset >= mappingCount_) return false;
  const uint32_t resolved =
      static_cast<uint32_t>(firstPhysicalPage_) + offset;
  if (resolved > UINT16_MAX) return false;
  physicalPage = static_cast<uint16_t>(resolved);
  return true;
}

}  // namespace att
