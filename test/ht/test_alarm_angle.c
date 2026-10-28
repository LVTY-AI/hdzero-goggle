#include <limits.h>
#include <stdio.h>

#include "core/ht_alarm.h"

static int failures;

static void check(int angle, bool want, const char *what) {
    if (ht_alarm_angle_is_set(angle) != want) {
        fprintf(stderr, "FAIL: %s (%d) reads as %s\n", what, angle, want ? "not set" : "set");
        failures++;
    }
}

int main(void) {
    // Every angle Set Alarm Angle can store reads as set.
    for (int angle = -180; angle <= 180; angle++)
        check(angle, true, "a stored tilt");

    // Nothing else does: the default in settings.c, and anything out of range.
    check(1300, false, "the default");
    check(181, false, "one past the top of the range");
    check(-181, false, "one past the bottom of the range");
    check(INT_MAX, false, "the largest value");
    check(INT_MIN, false, "the smallest value");

    if (failures)
        return 1;
    puts("test_alarm_angle: all checks passed");
    return 0;
}
