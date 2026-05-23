#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(HDZBOXPRO)

typedef enum {
    PROTOCOL_NONE   = 0,
    PROTOCOL_HDZ    = 1,
    PROTOCOL_ANALOG = 2,
} scan_protocol_t;

// HDZ band values match band_t from page_scannow.h (RACE_BAND=0, LOW_BAND=1).
// Analog channel indices are 0..47 in band order A, B, E, F, R, L (matches rtc6715 tab[]).
typedef struct {
    uint16_t freq_mhz;       // e.g. 5658
    int8_t   hdz_band;       // 0=RACE, 1=LOW, -1=N/A
    int8_t   hdz_channel;    // 0..11, -1=N/A
    int8_t   analog_channel; // 0..47, -1=N/A
} scan_freq_entry_t;

typedef struct {
    scan_protocol_t protocol;
    uint8_t  strength;   // normalized 0..100
    uint8_t  hdz_gain;   // raw, valid when protocol == PROTOCOL_HDZ
    uint16_t analog_mv;  // raw, valid when protocol == PROTOCOL_ANALOG
} scan_result_t;

extern const scan_freq_entry_t scan_freq_table[];
extern const size_t            scan_freq_table_len;

bool scan_probe_hdzero(uint8_t band, uint8_t channel,
                       uint8_t *gain_out, bool *valid_out);

bool scan_probe_analog(uint8_t channel_idx,
                       uint16_t *rssi_mv_out, bool *valid_out);

scan_result_t scan_probe_both(const scan_freq_entry_t *entry);

void scan_core_self_check(void);
void scan_core_idle_tick(void);
void scan_core_notify_analog_powered_on(void);

#endif // HDZBOXPRO

#ifdef __cplusplus
}
#endif
