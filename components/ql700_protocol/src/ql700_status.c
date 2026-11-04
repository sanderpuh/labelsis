/* QL-700 status decoder. Offsets from Brother QL-series raster reference,
 * Section 4.2, and the published QL protocol implementation notes.
 */
#include "ql700_status.h"

bool ql700_status_decode(const uint8_t *frame, size_t len,
                         ql700_status_t *out)
{
    if (!frame || !out || len != 32) return false;
    /* Fixed head mark, frame length, 'B', QL series '4', QL-700 '5'. */
    if (frame[0] != 0x80 || frame[1] != 0x20 ||
        frame[2] != 0x42 || frame[3] != 0x34 ||
        frame[4] != 0x35) return false;

    ql700_status_t decoded = {0};
    decoded.error1          = frame[8];
    decoded.error2          = frame[9];
    decoded.media_width_mm  = frame[10];
    decoded.media_type      = frame[11];
    decoded.media_length_mm = frame[17];
    decoded.status_type     = frame[18];
    decoded.phase_type      = frame[19];
    decoded.phase_number    = (uint16_t)(((uint16_t)frame[20] << 8) | frame[21]);
    decoded.notification    = frame[22];
    *out = decoded;
    return true;
}

const char *ql700_media_kind(uint8_t type)
{
    switch (type) {
    case 0x00: return "none";
    case 0x0a: return "continuous";
    case 0x0b: return "die_cut";
    default:   return "unknown";
    }
}

const char *ql700_primary_error(const ql700_status_t *s)
{
    if (!s) return "unknown";
    /* Prioritize actionable problems, but always expose both raw masks. */
    if (s->error2 & 0x80) return "system_error";
    if (s->error2 & 0x10) return "cover_open";
    if (s->error1 & 0x04) return "cutter_jam";
    if (s->error1 & 0x01) return "no_media";
    if (s->error1 & 0x02) return "end_of_media";
    if (s->error2 & 0x01) return "replace_media";
    if (s->error2 & 0x40) return "media_cannot_feed";
    if (s->error2 & 0x04) return "communication_error";
    if (s->error2 & 0x02) return "expansion_buffer_full";
    if (s->error2 & 0x08) return "communication_buffer_full";
    if (s->error1 & 0x10) return "printer_in_use";
    if (s->error1 & 0x20) return "printer_off";
    if (s->error1 & 0x80) return "fan_motor_error";
    if (s->error1 & 0x40) return "high_voltage";
    if (s->error1 || s->error2) return "other_error";
    return s->media_type == 0x00 ? "no_media" : "none";
}
