#include <string.h>
#include "edid.h"

// The base block holds four 18-byte descriptors: detailed timings (first two
// bytes non-zero) or display descriptors tagged in byte 3.
#define DESC(buf, i)   (&(buf)[54 + (i) * 18])
#define IS_TIMING(d)   ((d)[0] || (d)[1])
#define TAG_NAME       0xFC
#define TAG_SERIAL     0xFF

static bool isBaseBlock(const u8 *buf, size_t len) {
    static const u8 hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    return len >= 128 && memcmp(buf, hdr, sizeof(hdr)) == 0;
}

bool edidParse(const u8 *buf, size_t len, EdidInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!isBaseBlock(buf, len)) return false;
    out->screen_w_cm = buf[21];
    out->screen_h_cm = buf[22];
    for (int i = 0; i < 4; i++) {
        const u8 *d = DESC(buf, i);
        if (IS_TIMING(d)) {
            if (i == 0) {   // the first descriptor is the preferred mode
                out->native_w = d[2] | ((d[4] & 0xF0) << 4);
                out->native_h = d[5] | ((d[7] & 0xF0) << 4);
            }
        } else if (d[3] == TAG_NAME) {   // text, ended by 0x0A when shorter than 13
            for (int k = 0; k < 13 && d[5 + k] != 0x0A; k++) out->name[k] = (char)d[5 + k];
        }
    }
    return true;
}

void edidScrubSerial(u8 *buf, size_t len) {
    if (!isBaseBlock(buf, len)) return;
    memset(buf + 12, 0, 4);
    for (int i = 0; i < 4; i++) {
        u8 *d = DESC(buf, i);
        if (!IS_TIMING(d) && d[3] == TAG_SERIAL) {
            d[5] = 0x0A;
            memset(d + 6, ' ', 12);
        }
    }
    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum += buf[i];
    buf[127] = (u8)-sum;
}
