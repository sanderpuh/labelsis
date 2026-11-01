/* QL-700-only trial raster encoder.
 * Source: Brother QL-500/550/560/570/580N/650TD/700/1050/1060N
 * Raster Command Reference, sections 3.2 and 5.
 * The QL-700 requires 90 uncompressed bytes per raster line.
 * Its 62 mm continuous label has 12 unprintable head pins at each edge.
 */
#include "ql700_raster.h"
#include <string.h>

static void set_dot(uint8_t row[QL700_ROW_BYTES], unsigned x)
{
    if (x < QL700_62MM_LEFT_DOTS ||
        x >= QL700_62MM_LEFT_DOTS + QL700_62MM_PRINT_DOTS) return;
    row[x / 8u] |= (uint8_t)(0x80u >> (x % 8u));
}

/* Black border and two diagonal lines within the printable 696-dot area.
 * Intentionally obvious even when the alignment is slightly wrong.
 */
void ql700_test_row(uint16_t y, uint8_t out[QL700_ROW_BYTES])
{
    memset(out, 0, QL700_ROW_BYTES);
    if (y >= QL700_TEST_ROWS) return;
    const unsigned l = QL700_62MM_LEFT_DOTS + 24u;
    const unsigned r = QL700_62MM_LEFT_DOTS + QL700_62MM_PRINT_DOTS - 25u;
    if ((y >= 24u && y < 31u) ||
        (y >= QL700_TEST_ROWS - 31u && y < QL700_TEST_ROWS - 24u)) {
        for (unsigned x = l; x <= r; ++x) set_dot(out, x);
    }
    if (y >= 24u && y < QL700_TEST_ROWS - 24u) {
        for (unsigned dx = 0; dx < 5; ++dx) {
            set_dot(out, l + dx);
            set_dot(out, r - dx);
        }
        unsigned diag = l + ((r - l) * (unsigned)y / (QL700_TEST_ROWS - 1u));
        for (unsigned dx = 0; dx < 4; ++dx) set_dot(out, diag + dx);
    }
}

bool ql700_send_test_page_62mm(ql700_write_fn write, void *ctx)
{
    if (!write) return false;

    /* Invalidate any partially received prior job, then initialize.
     * QL-700 is raster-by-default; unlike some other QL models, Brother's
     * QL-700 command reference does not list the ESC i a mode switch.
     */
    const uint8_t nul[200] = {0};
    const uint8_t initialize[] = {0x1b, 0x40};
    if (!write(ctx, nul, sizeof nul) ||
        !write(ctx, initialize, sizeof initialize)) return false;

    /* ESC i z: type and width valid; recovery enabled; 62 mm continuous;
     * raster count = 360, little endian; first and only page.
     */
    const uint8_t print_info[] = {
        0x1b, 0x69, 0x7a, 0x86, 0x0a, 62, 0,
        (uint8_t)(QL700_TEST_ROWS & 0xffu),
        (uint8_t)((QL700_TEST_ROWS >> 8) & 0xffu), 0, 0, 0, 0
    };
    const uint8_t auto_cut[] = {0x1b, 0x69, 0x4d, 0x40};
    const uint8_t cut_every_label[] = {0x1b, 0x69, 0x41, 1};
    const uint8_t expanded[] = {0x1b, 0x69, 0x4b, 0x08};
    const uint8_t margin[] = {0x1b, 0x69, 0x64, 35, 0};
    if (!write(ctx, print_info, sizeof print_info) ||
        !write(ctx, auto_cut, sizeof auto_cut) ||
        !write(ctx, cut_every_label, sizeof cut_every_label) ||
        !write(ctx, expanded, sizeof expanded) ||
        !write(ctx, margin, sizeof margin)) return false;

    /* No compression: each raster line is 'g 00 5A' + 90 data bytes. */
    uint8_t raster[3 + QL700_ROW_BYTES] = {0x67, 0, QL700_ROW_BYTES};
    for (uint16_t y = 0; y < QL700_TEST_ROWS; ++y) {
        ql700_test_row(y, raster + 3);
        if (!write(ctx, raster, sizeof raster)) return false;
    }

    /* Final page MUST use SUB / Control-Z (1Ah), not FF (0Ch). */
    const uint8_t finish[] = {0x1a};
    return write(ctx, finish, sizeof finish);
}
