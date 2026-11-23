#include "time_rules.h"
namespace att {
int32_t dayNumber(uint32_t epoch, int16_t off) {
  int64_t local = (int64_t)epoch + (int64_t)off * 60 - (int64_t)EPOCH_2020;
  if (local < 0) return -1;
  return (int32_t)(local / 86400);
}
uint8_t weekdayIndex(int32_t day) { return (uint8_t)((day + 2) % 7); }
uint16_t minuteOfDay(uint32_t epoch, int16_t off) {
  int64_t s = ((int64_t)epoch + (int64_t)off * 60) % 86400;
  if (s < 0) s += 86400;
  return (uint16_t)(s / 60);
}
namespace {
bool leapYear(uint16_t year) {
  return (year % 4u == 0u) &&
      ((year % 100u != 0u) || (year % 400u == 0u));
}
uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t days[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && leapYear(year) ? 29 : days[month - 1];
}
}  // namespace
bool monthKeyForEpoch(uint32_t epochUtc, int16_t off, uint32_t& outKey) {
  if (off < -720 || off > 840) return false;
  const int64_t localEpoch = static_cast<int64_t>(epochUtc) +
      static_cast<int64_t>(off) * 60;
  if (localEpoch < static_cast<int64_t>(EPOCH_2020)) return false;
  uint64_t remainingDays = static_cast<uint64_t>(
      localEpoch - static_cast<int64_t>(EPOCH_2020)) / 86400ULL;
  uint16_t year = 2020;
  while (year < 2200) {
    const uint16_t yearDays = leapYear(year) ? 366u : 365u;
    if (remainingDays < yearDays) break;
    remainingDays -= yearDays;
    ++year;
  }
  if (year >= 2200) return false;
  uint8_t month = 1;
  while (month < 12) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (remainingDays < monthDays) break;
    remainingDays -= monthDays;
    ++month;
  }
  outKey = static_cast<uint32_t>(year) * 100u + month;
  return true;
}
uint32_t dayStartEpoch(int32_t day, int16_t off) {
  return (uint32_t)((int64_t)EPOCH_2020 + (int64_t)day * 86400 - (int64_t)off * 60);
}
bool validateTimetable(const Timetable& t, uint16_t lead) {
  for (int d = 0; d < 5; d++) {
    for (int s = 0; s < 2; s++) {
      const Slot& x = t.day[d][s];
      if (!x.enabled) continue;
      if (x.end <= x.start || x.end > 1439) return false;
      if (x.start < lead) return false;
    }
    const Slot& a = t.day[d][0];
    const Slot& b = t.day[d][1];
    if (a.enabled && b.enabled && (int)b.start - (int)lead < (int)a.end) return false;
  }
  return true;
}
int sessionAt(const Timetable& t, const Params& p, uint32_t epoch) {
  int32_t day = dayNumber(epoch, p.utcOffsetMin);
  if (day < 0) return -1;
  uint8_t wd = weekdayIndex(day);
  if (wd > 4) return -1;
  int m = minuteOfDay(epoch, p.utcOffsetMin);
  for (int s = 0; s < 2; s++) {
    const Slot& x = t.day[wd][s];
    if (x.enabled && m + (int)p.scanLeadMin >= (int)x.start && m < (int)x.end) return s;
  }
  return -1;
}
bool isLate(const Slot& s, uint16_t lateMin, uint16_t minute) { return (uint32_t)minute > (uint32_t)s.start + lateMin; }
uint8_t correctionSecondsRemaining(uint32_t nowEpoch, uint32_t changedAtEpoch) {
  if (changedAtEpoch == 0 || nowEpoch < changedAtEpoch) return 0;
  const uint32_t elapsed = nowEpoch - changedAtEpoch;
  if (elapsed >= 60u) return 0;
  return static_cast<uint8_t>(60u - elapsed);
}
bool hasStarted(const Slot& s, uint16_t minute) { return minute >= s.start; }
bool hasEnded(const Slot& s, uint16_t minute) { return minute >= s.end; }
Timetable defaultTimetable() {
  Timetable t;
  for (int d = 0; d < 5; d++) {
    t.day[d][0] = Slot{480, 750, true};
    t.day[d][1] = Slot{810, 990, true};
  }
  return t;
}
}
