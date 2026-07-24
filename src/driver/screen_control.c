#include "screen.h"

#include <pthread.h>

static pthread_mutex_t screen_control_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool requested_on = true;
static uint32_t inhibit_mask;
static bool request_seen;
static bool effective_known;
static bool effective_on;

static void apply_effective_locked(void) {
    bool desired_on = requested_on && (inhibit_mask == 0);

    // The panel backend is not ready until the first normal display request.
    // This allows a development hold to be latched during early input setup
    // without touching uninitialized hardware.
    if (!request_seen || !screen.display_raw || (effective_known && effective_on == desired_on))
        return;

    screen.display_raw(desired_on);
    effective_on = desired_on;
    effective_known = true;
}

void screen_display_request(bool on) {
    pthread_mutex_lock(&screen_control_mutex);
    requested_on = on;
    request_seen = true;
    apply_effective_locked();
    pthread_mutex_unlock(&screen_control_mutex);
}

void screen_set_inhibited(uint32_t reasons, bool inhibited) {
    if (reasons == 0)
        return;

    pthread_mutex_lock(&screen_control_mutex);
    if (inhibited)
        inhibit_mask |= reasons;
    else
        inhibit_mask &= ~reasons;
    apply_effective_locked();
    pthread_mutex_unlock(&screen_control_mutex);
}

bool screen_inhibit_active(uint32_t reasons) {
    bool active;

    pthread_mutex_lock(&screen_control_mutex);
    active = reasons != 0 && (inhibit_mask & reasons) == reasons;
    pthread_mutex_unlock(&screen_control_mutex);
    return active;
}

bool screen_effective_on(void) {
    bool on;

    pthread_mutex_lock(&screen_control_mutex);
    on = request_seen ? effective_on : (requested_on && inhibit_mask == 0);
    pthread_mutex_unlock(&screen_control_mutex);
    return on;
}
