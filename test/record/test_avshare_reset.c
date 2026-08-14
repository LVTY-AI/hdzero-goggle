#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include the implementation so this focused test can seed and inspect the
// private shared-ring bookkeeping without expanding the production API.
#include "../../src/record/avshare.c"

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void) {
    uint8_t shared[MEDIA_NUM] = {1, 1};
    ShareStream_t headers[MEDIA_NUM][SHAREBUF_COUNT];

    memset(&gAvshare, 0, sizeof(gAvshare));
    memset(headers, 0, sizeof(headers));
    gAvshare.bufShare = shared;
    gAvshare.ptrConnect = shared;

    for (int media = 0; media < MEDIA_NUM; media++) {
        gAvshare.bufStreamHead[media] = headers[media];
        gAvshare.idxNext[media] = 17 + media;
    }

    avshare_reset();

    for (int media = 0; media < MEDIA_NUM; media++) {
        check(gAvshare.idxNext[media] == 0, "reset must rewind the writer ring");
        check(gAvshare.ptrConnect[media] == 1,
              "reset must preserve the active RTSP connection");
        for (int slot = 0; slot < SHAREBUF_COUNT; slot++) {
            check(gAvshare.bufStreamHead[media][slot].wflag == 1,
                  "reset must discard every queued stream chunk");
        }
    }

    puts("test_avshare_reset: all checks passed");
    return 0;
}
