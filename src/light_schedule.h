#pragma once

#include <stdint.h>

namespace light_schedule {

constexpr int64_t SECONDS_PER_DAY = 24LL * 60 * 60;

constexpr int64_t wrapDay(int64_t seconds) {
    return ((seconds % SECONDS_PER_DAY) + SECONDS_PER_DAY) % SECONDS_PER_DAY;
}

constexpr int minuteOfDay(int64_t utcEpochSeconds, int32_t utcOffsetSeconds) {
    return static_cast<int>(wrapDay(utcEpochSeconds + utcOffsetSeconds) / 60);
}

constexpr bool isOnAtMinute(int minute, int onMinute, int offMinute) {
    return onMinute < offMinute ? minute >= onMinute && minute < offMinute
                               : minute >= onMinute || minute < offMinute;
}

constexpr bool isOnAt(int64_t utcEpochSeconds, int32_t utcOffsetSeconds, int onMinute,
                      int offMinute) {
    return isOnAtMinute(minuteOfDay(utcEpochSeconds, utcOffsetSeconds), onMinute, offMinute);
}

}  // namespace light_schedule