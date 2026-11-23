#include "as608_runtime.h"

namespace att {

As608Runtime::As608Runtime(Stream& stream, RosterStore& roster,
    uint32_t sensorPassword, const ISensorPageMap* pageMap)
    : arbiter_(),
      link_(stream, arbiter_),
      ownedOccupancy_(),
      occupancy_(&ownedOccupancy_),
      enrollmentSensor_(link_, sensorPassword, AS608_OWNER_ENROLLMENT),
      occupancyReader_(enrollmentSensor_, *occupancy_, pageMap),
      enrollmentFsm_(roster, *occupancy_, enrollmentSensor_, pageMap),
      enrollment_(arbiter_, enrollmentFsm_, enrollmentSensor_, occupancyReader_, true),
      attendance_(arbiter_, link_, sensorPassword, true) {}

As608Runtime::As608Runtime(Stream& stream, RosterStore& roster,
    Occupancy& occupancy, uint32_t sensorPassword,
    const ISensorPageMap* pageMap)
    : arbiter_(),
      link_(stream, arbiter_),
      ownedOccupancy_(),
      occupancy_(&occupancy),
      enrollmentSensor_(link_, sensorPassword, AS608_OWNER_ENROLLMENT),
      occupancyReader_(enrollmentSensor_, *occupancy_, pageMap),
      enrollmentFsm_(roster, *occupancy_, enrollmentSensor_, pageMap),
      enrollment_(arbiter_, enrollmentFsm_, enrollmentSensor_, occupancyReader_, true),
      attendance_(arbiter_, link_, sensorPassword, true) {}

}  // namespace att
