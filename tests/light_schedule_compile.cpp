#include "light_schedule.h"

namespace {

constexpr int DEFAULT_ON = 8 * 60;
constexpr int DEFAULT_OFF = 18 * 60;

constexpr int64_t utcForLocalMinute(int localMinute, int32_t offsetSeconds) {
    return static_cast<int64_t>(localMinute) * 60 - offsetSeconds;
}

static_assert(!light_schedule::isOnAt(utcForLocalMinute(7 * 60 + 59, 10 * 3600), 10 * 3600,
                                      DEFAULT_ON, DEFAULT_OFF),
              "lights must be off immediately before the morning boundary");
static_assert(light_schedule::isOnAt(utcForLocalMinute(DEFAULT_ON, 10 * 3600), 10 * 3600,
                                     DEFAULT_ON, DEFAULT_OFF),
              "lights must turn on at 08:00 local");
static_assert(light_schedule::isOnAt(utcForLocalMinute(17 * 60 + 59, 10 * 3600), 10 * 3600,
                                     DEFAULT_ON, DEFAULT_OFF),
              "lights must remain on immediately before the evening boundary");
static_assert(!light_schedule::isOnAt(utcForLocalMinute(DEFAULT_OFF, 10 * 3600), 10 * 3600,
                                      DEFAULT_ON, DEFAULT_OFF),
              "lights must turn off at 18:00 local");

static_assert(light_schedule::minuteOfDay(22 * 3600, 10 * 3600) == DEFAULT_ON,
              "positive offsets must wrap into the next local day");
static_assert(light_schedule::minuteOfDay(13 * 3600 + 30 * 60, -(5 * 3600 + 30 * 60)) ==
                  DEFAULT_ON,
              "negative half-hour offsets must be applied exactly");
static_assert(light_schedule::minuteOfDay(60 * 60, -2 * 3600) == 23 * 60,
              "negative local times must wrap into the previous day");

constexpr int OVERNIGHT_ON = 18 * 60;
constexpr int OVERNIGHT_OFF = 8 * 60;
static_assert(!light_schedule::isOnAtMinute(17 * 60 + 59, OVERNIGHT_ON, OVERNIGHT_OFF),
              "overnight schedule must be off before its evening boundary");
static_assert(light_schedule::isOnAtMinute(OVERNIGHT_ON, OVERNIGHT_ON, OVERNIGHT_OFF),
              "overnight schedule must turn on at its evening boundary");
static_assert(light_schedule::isOnAtMinute(0, OVERNIGHT_ON, OVERNIGHT_OFF),
              "overnight schedule must remain on through midnight");
static_assert(light_schedule::isOnAtMinute(7 * 60 + 59, OVERNIGHT_ON, OVERNIGHT_OFF),
              "overnight schedule must remain on before its morning boundary");
static_assert(!light_schedule::isOnAtMinute(OVERNIGHT_OFF, OVERNIGHT_ON, OVERNIGHT_OFF),
              "overnight schedule must turn off at its morning boundary");

}  // namespace

int main() {
    return 0;
}