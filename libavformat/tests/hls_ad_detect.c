/*
 * Regression tests for parsed HLS ad metadata.
 * Copyright (c) 2026 FongMi
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavformat/hls_ad_detect.h"

static int check(const FFHLSAdSegment *segments, int count,
                 int has_cue, int valid_cue, int allow_repeated,
                 int expected_count, const uint8_t *expected)
{
    uint8_t *remove = calloc(count, sizeof(*remove));
    int actual;
    int failed;

    if (!remove)
        return 1;
    actual = ff_hls_ad_detect(segments, count, has_cue, valid_cue,
                              allow_repeated, remove);
    failed = actual != expected_count || memcmp(remove, expected, count);
    if (failed) {
        fprintf(stderr, "HLS ad detection mismatch: got %d, expected %d\n",
                actual, expected_count);
    }
    free(remove);
    return failed;
}

int main(void)
{
    FFHLSAdSegment cue[] = {
        {.url = "opening.ts", .duration = 4000000, .size = -1},
        {.url = "ad.ts", .duration = 2000000, .size = -1, .cue_ad = 1},
        {.url = "movie.ts", .duration = 5000000, .size = -1},
    };
    FFHLSAdSegment repeat[] = {
        {.url = "intro1.ts", .duration = 2000000, .size = -1},
        {.url = "intro2.ts", .duration = 2000000, .size = -1},
        {.url = "movie.ts", .duration = 5000000, .size = -1, .discontinuity = 1},
        {.url = "intro1.ts", .duration = 2000000, .size = -1, .discontinuity = 1},
        {.url = "intro2.ts", .duration = 2000000, .size = -1},
    };
    const uint8_t cue_mask[] = {0, 1, 0};
    const uint8_t repeat_mask[] = {1, 1, 0, 1, 1};
    const uint8_t none3[] = {0, 0, 0};
    const uint8_t none5[] = {0, 0, 0, 0, 0};
    FFHLSAdSegment measured_segments[] = {
        {.url = "before.ts", .duration = 4000000},
        {.url = "ad1.ts", .duration = 4000000, .discontinuity = 1},
        {.url = "ad2.ts", .duration = 4000000},
        {.url = "after.ts", .duration = 4000000, .discontinuity = 1},
    };
    FFHLSAdProbeResult measured[] = {
        {.size = 400000, .frame_rate = 24, .width = 1920, .height = 1080},
        {.size = 400000, .frame_rate = 24, .width = 1280, .height = 720},
        {.size = 400000, .frame_rate = 24, .width = 1280, .height = 720},
        {.size = 400000, .frame_rate = 24, .width = 1920, .height = 1080},
    };
    FFHLSAdSegment replay_segments[7] = {0};
    FFHLSAdProbeResult replay_results[7] = {0};
    FFHLSAdSegment single[] = {
        {.url = "card.ts", .duration = 1000000, .size = -1},
        {.url = "movie.ts", .duration = 4000000, .size = -1, .discontinuity = 1},
        {.url = "card.ts", .duration = 1000000, .size = -1, .discontinuity = 1},
    };
    FFHLSAdSegment opening_cue[] = {
        {.url = "ad0.ts", .duration = 2000000, .size = -1, .cue_ad = 1},
        {.url = "ad1.ts", .duration = 2000000, .size = -1, .cue_ad = 1},
        {.url = "movie.ts", .duration = 4000000, .size = -1, .discontinuity = 1},
    };
    static const char *const ad_urls[] = {
        "ad0.ts", "ad1.ts", "ad2.ts", "ad3.ts", "ad4.ts",
        "ad5.ts", "ad6.ts", "ad7.ts", "ad8.ts",
    };
    FFHLSAdSegment repeated_opening[20] = {0};
    uint8_t repeated_opening_mask[20] = {0};
    const uint8_t opening_cue_mask[] = {1, 1, 0};
    const uint8_t none20[20] = {0};

    for (int i = 0; i < 9; i++) {
        repeated_opening[i] = (FFHLSAdSegment) {
            .url = ad_urls[i],
            .duration = i == 1 ? 4840000 : 3000000,
            .size = -1,
        };
        repeated_opening[10 + i] = repeated_opening[i];
        repeated_opening_mask[i] = repeated_opening_mask[10 + i] = 1;
    }
    repeated_opening[9] = (FFHLSAdSegment) {
        .url = "movie0.ts", .key = "content.key", .duration = 4000000,
        .size = -1, .key_type = 1, .discontinuity = 1,
    };
    repeated_opening[10].discontinuity = 1;
    repeated_opening[19] = (FFHLSAdSegment) {
        .url = "movie1.ts", .key = "content.key", .duration = 4000000,
        .size = -1, .key_type = 1, .discontinuity = 1,
    };

    if (check(cue, 3, 1, 1, 1, 1, cue_mask) ||
        check(cue, 3, 1, 0, 1, 0, none3) ||
        check(cue, 3, 0, 1, 1, 0, none3) ||
        check(repeat, 5, 0, 1, 1, 4, repeat_mask) ||
        check(repeat, 5, 0, 1, 0, 0, none5) ||
        check(single, 3, 0, 1, 1, 0, none3) ||
        check(opening_cue, 3, 1, 1, 1, 2, opening_cue_mask) ||
        check(repeated_opening, 20, 0, 1, 1, 18, repeated_opening_mask))
        return 1;

    repeated_opening[10].duration++;
    if (check(repeated_opening, 20, 0, 1, 1, 0, none20))
        return 1;

    cue[0].cue_ad = cue[2].cue_ad = 1;
    if (check(cue, 3, 1, 1, 1, 0, none3))
        return 1;

    repeat[3].offset = 100;
    if (check(repeat, 5, 0, 1, 1, 0, none5))
        return 1;
    repeat[3].offset = 0;
    repeat[3].iv[15] = 1;
    repeat[0].key_type = repeat[3].key_type = 1;
    if (check(repeat, 5, 0, 1, 1, 0, none5))
        return 1;

    if (!ff_hls_ad_candidate_start(measured_segments, measured, 4, 1) ||
        !ff_hls_ad_candidate_boundaries(measured_segments, measured, 4, 1, 3) ||
        !ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3) ||
        ff_hls_ad_confirm_window(measured_segments, measured, 4, 0, 3))
        return 1;
    measured[2].size = 0;
    if (!ff_hls_ad_candidate_boundaries(measured_segments, measured, 4, 1, 3) ||
        ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3))
        return 1;
    measured[2].size = 400000;
    measured[1] = measured[0];
    if (ff_hls_ad_candidate_start(measured_segments, measured, 4, 1) ||
        ff_hls_ad_candidate_boundaries(measured_segments, measured, 4, 1, 3))
        return 1;
    measured[1].width = measured[2].width = 1280;
    measured[1].height = measured[2].height = 720;
    measured[2].width = 1920;
    if (ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3))
        return 1;
    measured[1].width = measured[2].width = 1920;
    measured[1].height = measured[2].height = 1080;
    measured[1].frame_rate = measured[2].frame_rate = 30;
    if (!ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3))
        return 1;
    measured[1].frame_rate = measured[2].frame_rate = 24;
    measured[1].size = measured[2].size = 100000;
    if (!ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3))
        return 1;
    measured[3].size = 800000;
    if (ff_hls_ad_confirm_window(measured_segments, measured, 4, 1, 3))
        return 1;
    for (int i = 0; i < 7; i++) {
        replay_segments[i].duration = 4000000;
        replay_results[i] = (FFHLSAdProbeResult) {
            .size = 4000, .frame_rate = 25, .width = 1280, .height = 720,
            .has_content_fingerprint = 1,
        };
    }
    replay_results[1].content_fingerprint[0] =
        replay_results[4].content_fingerprint[0] = 1;
    replay_results[2].content_fingerprint[0] =
        replay_results[5].content_fingerprint[0] = 2;
    if (!ff_hls_ad_same_content(replay_segments, replay_results, 7, 1, 3, 4, 6) ||
        ff_hls_ad_same_content(replay_segments, replay_results, 7, 1, 2, 4, 5))
        return 1;
    replay_results[5].content_fingerprint[0] = 9;
    if (ff_hls_ad_same_content(replay_segments, replay_results, 7, 1, 3, 4, 6))
        return 1;
    replay_results[5].content_fingerprint[0] = 2;
    replay_results[5].has_content_fingerprint = 0;
    if (ff_hls_ad_same_content(replay_segments, replay_results, 7, 1, 3, 4, 6))
        return 1;
    replay_results[5].has_content_fingerprint = 1;
    replay_segments[5].duration++;
    if (ff_hls_ad_same_content(replay_segments, replay_results, 7, 1, 3, 4, 6))
        return 1;
    return 0;
}
