#include "ql700_raster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP (200 + 2 + 13 + 4 + 4 + 4 + 5 + (QL700_TEST_ROWS * 93) + 1)
static uint8_t wire[CAP];
static size_t used;
static unsigned writes;
static unsigned fail_after;

static bool capture(void *ctx, const uint8_t *bytes, size_t n)
{
    (void)ctx;
    if (fail_after && ++writes == fail_after) return false;
    if (!bytes || used + n > CAP) return false;
    memcpy(wire + used, bytes, n);
    used += n;
    return true;
}
int main(void)
{
    assert(!ql700_send_test_page_62mm(NULL, NULL));
    assert(ql700_send_test_page_62mm(capture, NULL));
    assert(used == CAP);
    for (unsigned i=0;i<200;i++) assert(wire[i] == 0);
    assert(wire[200] == 0x1b && wire[201] == '@');
    assert(memcmp(wire+202, "\x1b\x69\x7a", 3) == 0);
    assert(wire[205] == 0x86 && wire[206] == 0x0a && wire[207] == 62);
    assert(wire[209] == 0x68 && wire[210] == 0x01); /* 360 lines */
    assert(wire[CAP-1] == 0x1a);
    const size_t raster_off = 200+2+13+4+4+4+5;
    for (unsigned y=0;y<QL700_TEST_ROWS;y++) {
        const uint8_t *r = wire + raster_off + 93*y;
        assert(r[0] == 'g' && r[1] == 0 && r[2] == 90);
        /* Twelve unprintable bits are always clear at both sides. */
        assert(r[3] == 0 && (r[4] & 0xf0) == 0);
        assert(r[91] == 0 && (r[92] & 0x0f) == 0);
    }
    uint8_t row[90];
    ql700_test_row(24, row);
    assert(memcmp(row, (uint8_t[90]){0}, 90) != 0);
    ql700_test_row(QL700_TEST_ROWS, row);
    assert(memcmp(row, (uint8_t[90]){0}, 90) == 0);
    used = 0; writes = 0; fail_after = 3;
    assert(!ql700_send_test_page_62mm(capture, NULL));
    puts("QL-700 raster encoder tests passed");
    return 0;
}
