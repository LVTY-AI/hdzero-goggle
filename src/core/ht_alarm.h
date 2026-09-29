#pragma once

#include <stdbool.h>

// Set Alarm Angle stores the tilt it reads, -180 to 180 degrees. Any other
// value -- the default is 1300 -- means the angle has never been set, and the
// head alarm stays silent until it is.
static inline bool ht_alarm_angle_is_set(int angle) {
    return angle >= -180 && angle <= 180;
}
