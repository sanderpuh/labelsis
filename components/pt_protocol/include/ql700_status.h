#ifndef QL700_STATUS_H
#define QL700_STATUS_H

/* Brother QL-700 32-byte status response (300 dpi QL command family).
 * Protocol-only header: intentionally independent from ESP-IDF.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t error1;
    uint8_t error2;
    uint8_t media_width_mm;
    uint8_t media_type;       /* 0x4a continuous; 0x4b die-cut; 0 no media */
    uint8_t media_length_mm;  /* 0 for continuous */
    uint8_t status_type;
    uint8_t phase_type;
    uint16_t phase_number;
    uint8_t notification;
} ql700_status_t;

/* Reject malformed/non-QL-700 packets without changing *out. */
bool ql700_status_decode(const uint8_t *frame, size_t len,
                         ql700_status_t *out);
const char *ql700_media_kind(uint8_t media_type);
const char *ql700_primary_error(const ql700_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
