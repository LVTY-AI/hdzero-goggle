#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/screen.h"

static int raw_calls;
static bool raw_state;

static void fake_display_raw(bool on) {
    raw_calls++;
    raw_state = on;
}

screen_t screen = {
    .display = screen_display_request,
    .display_raw = fake_display_raw,
};

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        _Exit(1);
    }
}

int main(void) {
    // An early hold latches before the physical backend receives its first
    // request; this is how the development marker avoids a relaunch flash.
    screen_set_inhibited(SCREEN_INHIBIT_AUXILIARY, true);
    check(raw_calls == 0, "early inhibit must not touch the backend");
    check(!screen_effective_on(), "early inhibit must report dark");

    screen.display(true);
    check(raw_calls == 1 && !raw_state, "first request must apply the hold");

    screen_set_inhibited(SCREEN_INHIBIT_IDLE, true);
    screen_set_inhibited(SCREEN_INHIBIT_AUXILIARY, false);
    check(screen_inhibit_active(SCREEN_INHIBIT_IDLE), "idle hold remains active");
    check(raw_calls == 1, "releasing one hold must not relight the panel");

    screen_set_inhibited(SCREEN_INHIBIT_IDLE, false);
    check(raw_calls == 2 && raw_state, "last hold release must restore output");

    screen.display(false);
    screen.display(false);
    check(raw_calls == 3 && !raw_state, "duplicate off requests are idempotent");

    screen_set_inhibited(SCREEN_INHIBIT_SLEEP, true);
    screen.display(true);
    check(raw_calls == 3 && !raw_state, "sleep hold must suppress a later on request");
    screen_set_inhibited(SCREEN_INHIBIT_SLEEP, false);
    check(raw_calls == 4 && raw_state, "sleep release restores the latest request");

    puts("test_screen_control: all checks passed");
    return 0;
}
