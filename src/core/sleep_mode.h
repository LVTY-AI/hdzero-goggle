#pragma once

#include <stdbool.h>

// While they sleep, the goggles beep this often so they are not left on.
#define SLEEP_BEEP_INTERVAL_MS 20000

extern bool isSleeping;

void go_sleep();
void wake_up();
void sleep_reminder();
