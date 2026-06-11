#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int statusbar_init(void);
void statubar_update(void);
// Swap the RF source tag for "RF: Detecting..." while the blocking Auto
// Detect source entry probes; restore the normal tag when done.
void statusbar_source_detecting(bool on);

#ifdef __cplusplus
}
#endif
