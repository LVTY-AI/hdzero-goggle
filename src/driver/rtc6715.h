#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
typedef struct {
    void (*init)(bool power_on, bool audio_on);
    void (*set_ch)(int ch);
    int rssi;
} rtc6715_t;

extern void *thread_rtc6715_rssi(void *ptr);

// Defined in rtc6715.c under HDZBOXPRO || HDZGOGGLE2.
// Returns GPADC voltage in mV, or the last valid reading if GPADC returns -1.
int rtc6715_get_rssi(void);

extern rtc6715_t rtc6715;
#ifdef __cplusplus
}
#endif
