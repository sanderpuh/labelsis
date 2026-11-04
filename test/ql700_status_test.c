#include "ql700_status.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    uint8_t raw[32] = {0x80, 0x20, 0x42, 0x34, 0x35, 0x30};
    ql700_status_t s = {0};

    /* 62 mm continuous DK roll (DK-22205). */
    raw[10] = 62;
    raw[11] = 0x0a;
    raw[17] = 0;
    assert(ql700_status_decode(raw, sizeof raw, &s));
    assert(s.media_width_mm == 62 && s.media_length_mm == 0);
    assert(strcmp(ql700_media_kind(s.media_type), "continuous") == 0);
    assert(strcmp(ql700_primary_error(&s), "none") == 0);

    /* 29 x 90 mm die-cut roll, cover open. */
    raw[10] = 29;
    raw[11] = 0x0b;
    raw[17] = 90;
    raw[9] = 0x10;
    raw[18] = 0x02;
    raw[19] = 0x01;
    raw[20] = 0x12; raw[21] = 0x34;
    assert(ql700_status_decode(raw, sizeof raw, &s));
    assert(s.media_width_mm == 29 && s.media_length_mm == 90);
    assert(s.phase_number == 0x1234);
    assert(strcmp(ql700_media_kind(s.media_type), "die_cut") == 0);
    assert(strcmp(ql700_primary_error(&s), "cover_open") == 0);

    /* No media; error bit and no-media status can both identify it. */
    raw[9] = 0; raw[8] = 1; raw[10] = 0; raw[11] = 0;
    assert(ql700_status_decode(raw, sizeof raw, &s));
    assert(strcmp(ql700_primary_error(&s), "no_media") == 0);
    assert(strcmp(ql700_media_kind(s.media_type), "none") == 0);

    /* Bad frame must not mutate the output. */
    raw[4] = 0x38; /* QL-800, not QL-700 */
    s.media_width_mm = 123;
    assert(!ql700_status_decode(raw, sizeof raw, &s));
    assert(s.media_width_mm == 123);
    assert(!ql700_status_decode(raw, 31, &s));
    assert(!ql700_status_decode(NULL, 32, &s));

    puts("QL-700 status decoder tests passed");
    return 0;
}
