#ifndef QL700_RASTER_H
#define QL700_RASTER_H

/* QL-700 monochrome raster test-page encoder (pure C11, no ESP-IDF).
 * Firmware exposes this only behind an explicit test-print endpoint.
 * Supported media for this milestone: 62 mm CONTINUOUS DK roll ONLY.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QL700_HEAD_DOTS          720u
#define QL700_ROW_BYTES           90u
#define QL700_62MM_LEFT_DOTS      12u
#define QL700_62MM_PRINT_DOTS    696u
#define QL700_TEST_ROWS          360u  /* ~30.5 mm at 300 dpi */

/* Callback returns true only when the complete data block was transmitted. */
typedef bool (*ql700_write_fn)(void *ctx, const uint8_t *data, size_t size);

/* Build one complete, SINGLE-PAGE 62mm continuous-label print job.
 * Sends an approximately 30mm black test pattern, then auto-cuts.
 * Returns false if the transport rejects any block.
 * Does not wait for the printer's print-complete status message.
 */
bool ql700_send_test_page_62mm(ql700_write_fn write, void *ctx);

/* Deterministic 90-byte raster used by the print job; testable offline. */
void ql700_test_row(uint16_t row_index, uint8_t out[QL700_ROW_BYTES]);

#ifdef __cplusplus
}
#endif
#endif
