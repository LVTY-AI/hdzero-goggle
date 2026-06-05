#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)

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
    uint8_t  hdz_bw;     // bandwidth that locked, valid when protocol == PROTOCOL_HDZ
} scan_result_t;

extern const scan_freq_entry_t scan_freq_table[];
extern const size_t            scan_freq_table_len;

// Analog channel idx (0..47) → freq in MHz. Matches rtc6715 tab[] ordering
// (A, B, E, F, R, L). Use this when the analog index lookup in
// scan_freq_table needs a frequency fallback (some indices alias the same
// frequency and are merged — e.g. analog F8 idx=31 is at 5880 MHz, the same
// row as R7 idx=38).
extern const uint16_t scan_analog_idx_to_mhz[48];

// Returns the scan_freq_table index whose freq_mhz matches, or -1 if absent.
int scan_freq_table_find_by_mhz(uint16_t mhz);

// Number of positions in the radio/ELRS band-order channel sequence:
// A1-8, B1-8, E1-8, F1-8, R1-8, L1-8.
#define SCAN_BAND_ORDER_COUNT 48

// Map a band-order navigation index (0..SCAN_BAND_ORDER_COUNT-1, the A,B,E,F,R,L
// order the radio/ELRS backpack and the Auto Detect dial step through) to its
// scan_freq_table row, or NULL if absent. This differs from scan_freq_table's
// own strictly-ascending-frequency order. A..R use the analog band frequencies;
// L navigates the HDZero Lowband rows (the goggle's L band is HDZero Low, whose
// frequencies differ from analog L).
const scan_freq_entry_t *scan_band_order_entry(int idx);

bool scan_probe_hdzero(uint8_t band, uint8_t channel,
                       uint8_t *gain_out, bool *valid_out);

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
bool scan_probe_analog(uint8_t channel_idx,
                       uint16_t *rssi_mv_out, bool *valid_out);

scan_result_t scan_probe_both(const scan_freq_entry_t *entry);
#endif

// Fills out[] with the bandwidth(s) to sweep based on source.hdzero_bw:
// {wide,narrow} when Both, else the single configured bandwidth. Returns count.
int scan_hdz_bw_list(uint8_t out[2]);

// HDZ-only bandwidth sweep at one channel: opens+settles the baseband at each
// selected bandwidth (1 when Wide/Narrow, 2 when Both) and reports the one that
// locked with the strongest gain. Returns true if any locked; *out_bw gets the
// winning bandwidth (or the first tried if none). Leaves the baseband open at
// the last bandwidth tried.
bool scan_probe_hdzero_sweep(uint8_t band, uint8_t channel,
                             uint8_t *out_gain, uint8_t *out_bw);

// HDZ bandwidth re-acquire watchdog: call periodically (all targets). When
// viewing the HDZero source with BW=Auto and the signal drops (e.g. the VTX
// bandwidth changed Wide<->Narrow), re-sweeps Wide+Narrow at the current
// channel and reopens at whichever locks, so the picture returns without
// re-selecting the source. No-op unless those conditions hold.
void scan_core_hdz_bw_tick(void);

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Single-channel probe of both protocols that also honors the Wide/Narrow/Both
// bandwidth setting: for HDZ it opens+settles the baseband at each selected
// bandwidth and reports the one that locked in *out_bw (0/1). HDZ wins ties.
// Leaves the HDZ baseband open at the last bandwidth tried.
scan_result_t scan_probe_both_sweep(const scan_freq_entry_t *entry, uint8_t *out_bw);
#endif

void scan_core_self_check(void);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
void scan_core_idle_tick(void);
void scan_core_notify_analog_powered_on(void);
// Call after any non-scan_core code path powers off RTC6715 (e.g.
// app_switch_to_hdzero) so the next scan_probe_analog re-inits before
// tuning. Without this, scan_core's internal flag stays stale and the next
// probe reads garbage RSSI from the powered-off GPADC.
void scan_core_notify_analog_powered_off(void);
#endif

#endif // HDZBOXPRO

#ifdef __cplusplus
}
#endif
