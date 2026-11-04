#include "ql700_raster.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *wire;
static size_t wire_len;
static size_t wire_cap;
static unsigned writes;
static bool collect(void *unused, const uint8_t *data, size_t len)
{
    (void)unused;
    if (wire_len + len > wire_cap) return false;
    memcpy(wire + wire_len, data, len);
    wire_len += len;
    writes++;
    return true;
}
static bool fail_write(void *unused, const uint8_t *data, size_t len)
{
    (void)unused; (void)data; (void)len;
    return false;
}
int main(void)
{
    const unsigned n = QL700_BITMAP_MIN_ROWS;
    const unsigned max_n = QL700_BITMAP_MAX_ROWS;
    const size_t body_size = (size_t)max_n * QL700_ROW_BYTES;
    uint8_t *page = malloc(body_size);
    assert(page);
    memset(page, 0xff, body_size);
    wire_cap = 400 + (size_t)(max_n + 1) * (QL700_ROW_BYTES + 3);
    wire = malloc(wire_cap);
    assert(wire);
    assert(!ql700_send_bitmap_page_62mm(NULL, NULL, page, n));
    assert(!ql700_send_bitmap_page_62mm(collect, NULL, NULL, n));
    assert(!ql700_send_bitmap_page_62mm(collect, NULL, page, n - 1));
    assert(!ql700_send_bitmap_page_62mm(collect, NULL, page, QL700_BITMAP_MAX_ROWS + 1));
    assert(!ql700_send_bitmap_page_62mm(fail_write, NULL, page, n));
    assert(ql700_send_bitmap_page_62mm(collect, NULL, page, n));
    assert(writes == n + 8); /* seven preamble blocks + n rows + terminator */
    assert(wire_len == 200 + 2 + 13 + 4 + 4 + 4 + 5 + 93*n + 1);
    assert(wire[200] == 0x1b && wire[201] == 0x40);
    assert(wire[202] == 0x1b && wire[203] == 0x69 && wire[204] == 0x7a);
    assert(wire[209] == (n & 255) && wire[210] == (n >> 8));
    const size_t start = 200 + 2 + 13 + 4 + 4 + 4 + 5;
    assert(wire[start] == 0x67 && wire[start+1] == 0 && wire[start+2] == 90);
    assert(wire[start+3] == 0x00); /* head pins 0..7 */
    assert(wire[start+4] == 0x0f); /* head pins 8..11 clear */
    assert(wire[start+5] == 0xff); /* printable area preserved */
    assert(wire[start+3+88] == 0xf0);
    assert(wire[start+3+89] == 0x00);
    assert(wire[wire_len-1] == 0x1a);

    /* The configured 250 mm ceiling is accepted at its exact row limit. */
    wire_len = 0;
    writes = 0;
    assert(ql700_send_bitmap_page_62mm(collect, NULL, page, max_n));
    assert(writes == max_n + 8);
    assert(wire_len == 200 + 2 + 13 + 4 + 4 + 4 + 5 + 93*max_n + 1);
    assert(wire[209] == (max_n & 255));
    assert(wire[210] == (max_n >> 8));
    assert(wire[wire_len-1] == 0x1a);
    free(page); free(wire);
    puts("QL-700 bitmap encoder tests passed");
    return 0;
}
