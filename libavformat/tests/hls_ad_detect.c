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

#include <limits.h>
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
                              allow_repeated, NULL, remove);
    failed = actual != expected_count || memcmp(remove, expected, count);
    if (failed) {
        fprintf(stderr, "HLS ad detection mismatch: got %d, expected %d\n",
                actual, expected_count);
    }
    free(remove);
    return failed;
}

static int check_external_opening(void)
{
    static const char *const invalid_urls[] = {
        "https://example.com/movie/intro/0.ts",
        "https://example.com/movie/hls/intro/0.ts",
        "https://example.com/other%2fhls/0.ts",
        "https://example.com/other//hls/0.ts",
        "https://example.com/other/../else/hls/0.ts",
        "https://cdn.example.com/other/hls/0.ts",
        "https://example.com:8443/other/hls/0.ts",
        "https://example.com/other/hls/0.ts?token=1",
        "https://example.com/other/hls/0.ts#fragment",
        "https://example.com/other/hls/0.mp4",
        "https://example.com/third/hls/0.ts",
    };
    const char *base = "https://example.com/movie/hls/index.m3u8";
    FFHLSAdSegment segments[21], original[21];
    char urls[21][128];
    uint8_t remove[21];

    for (int i = 0; i < 21; i++) {
        snprintf(urls[i], sizeof(urls[i]), "https://example.com/%s/%d.ts",
                 i < 9 ? "other/hls" : "movie/hls", i);
        original[i] = (FFHLSAdSegment) {
            .url = urls[i], .duration = i == 8 ? 1760000 : 3000000,
            .size = -1, .discontinuity = i == 9,
        };
    }
    memcpy(segments, original, sizeof(segments));
    if (ff_hls_ad_detect(segments, 21, 0, 1, 1, base, remove) != 9)
        return 1;
    for (int i = 0; i < 21; i++)
        if (remove[i] != (i < 9))
            return 1;
    if (ff_hls_ad_detect(segments, 21, 0, 1, 1,
                          "https://EXAMPLE.com:443/movie/hls/index.m3u8", remove) != 9 ||
        ff_hls_ad_detect(segments, 21, 0, 1, 1, NULL, remove) ||
        ff_hls_ad_detect(segments, 21, 0, 1, 1,
                          "https://example.com/hls/index.m3u8", remove) ||
        ff_hls_ad_detect(segments, 10, 0, 1, 1, base, remove) ||
        ff_hls_ad_detect(segments, 21, 0, 1, 0, base, remove) ||
        ff_hls_ad_detect(segments, 21, 1, 0, 1, base, remove))
        return 1;
    for (size_t i = 0; i < sizeof(invalid_urls) / sizeof(*invalid_urls); i++) {
        segments[0].url = invalid_urls[i];
        if (ff_hls_ad_detect(segments, 21, 0, 1, 1, base, remove))
            return 1;
    }
    for (int i = 0; i < 10; i++) {
        memcpy(segments, original, sizeof(segments));
        switch (i) {
        case 0: segments[9].url = "https://example.com/else/hls/9.ts"; break;
        case 1: segments[12].discontinuity = 1; break;
        case 2: segments[0].key_type = 1; break;
        case 3: segments[0].init_url = "https://example.com/init.mp4"; break;
        case 4: segments[0].size = 188; break;
        case 5: segments[0].offset = 188; break;
        case 6: segments[0].duration = 0; break;
        case 7: segments[0].duration = INT64_MAX; break;
        case 8: segments[9].duration = INT64_MAX; break;
        case 9:
            for (int j = 0; j < 21; j++)
                segments[j].duration = INT64_MAX / 20;
            break;
        }
        if (ff_hls_ad_detect(segments, 21, 0, 1, 1, base, remove))
            return 1;
    }
    memcpy(segments, original, sizeof(segments));
    /* This rule does not restore the old 40-second cap. */
    for (int i = 0; i < 21; i++)
        segments[i].duration = 10000000;
    if (ff_hls_ad_detect(segments, 21, 0, 1, 1, base, remove) != 9)
        return 1;
    for (int i = 0; i < 9; i++)
        segments[i].duration = 20000000;
    return ff_hls_ad_detect(segments, 21, 0, 1, 1, base, remove) != 0;
}

static FFHLSAdProbeResult timed_result(int64_t start)
{
    return (FFHLSAdProbeResult) {
        .size = 400000, .frame_rate = 25, .width = 1920, .height = 1080,
        .video_timing = {start, start + 3960000, 40000, 40000},
        .audio_timing = {start, start + 3980000, 20000, 20000},
    };
}

static int check_timeline(void)
{
    FFHLSAdSegment segments[] = {
        {.url = "http://test/a.ts", .duration = 4000000, .size = -1},
        {.url = "http://test/b.ts", .duration = 4000000, .size = -1, .discontinuity = 1},
        {.url = "http://test/c.ts", .duration = 4000000, .size = -1},
        {.url = "http://test/d.ts", .duration = 4000000, .size = -1},
        {.url = "http://test/e.ts", .duration = 4000000, .size = -1, .discontinuity = 1},
    };
    FFHLSAdProbeResult results[5] = {0};
    const int64_t sizes[] = {1200000, 100000};

    results[0] = timed_result(100000000);
    results[1] = timed_result(1400000);
    results[3] = timed_result(9400000);
    results[4] = timed_result(104000000);
    /* The middle segment is not needed; both clocks resume after the block. */
    if (!ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4) ||
        !ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4) ||
        ff_hls_ad_confirm_timeline_window(segments, results, 5, 0, 4))
        return 1;
    /* High and low bitrates cannot hide an insertion when the baselines vary. */
    for (int i = 0; i < 2; i++) {
        results[1].size = sizes[i];
        results[4].size = 1000000;
        if (!ff_hls_ad_candidate_start(segments, results, 5, 1) ||
            ff_hls_ad_candidate_boundaries(segments, results, 5, 1, 4) ||
            !ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4) ||
            !ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
            return 1;
        results[4].audio_timing.first_pts += 20000;
        if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
            return 1;
        results[4].audio_timing.first_pts -= 20000;
        results[3].video_timing.last_pts -= 80000;
        if (ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
            return 1;
        results[3].video_timing.last_pts += 80000;
    }
    /* Invalid inserted timing cannot veto complete bitrate evidence. */
    results[2] = timed_result(5400000);
    results[4] = timed_result(104000000);
    for (int i = 1; i < 4; i++)
        results[i].size = 1200000;
    results[3].video_timing.last_pts -= 80000;
    if (ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4) ||
        !ff_hls_ad_confirm_window(segments, results, 5, 1, 4))
        return 1;
    results[1] = timed_result(1400000);
    results[2] = (FFHLSAdProbeResult){0};
    results[3] = timed_result(9400000);
    /* A one-segment block reuses the same head and tail measurement. */
    segments[2].discontinuity = 1;
    results[2] = timed_result(104000000);
    if (!ff_hls_ad_confirm_timeline_window(segments, results, 3, 1, 2))
        return 1;
    segments[2].discontinuity = 0;
    results[2] = (FFHLSAdProbeResult){0};
    /* A matching total span cannot hide an overlapping head and tail. */
    results[1].video_timing.last_pts += 8000000;
    results[1].audio_timing.last_pts += 8000000;
    if (!ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4) ||
        ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    results[1] = timed_result(1400000);
    segments[2].duration = 0;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].duration = 4000000;
    segments[2].key_type = 1;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].key_type = 0;
    segments[2].size = 400000;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].size = -1;
    segments[2].init_url = "init.mp4";
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].init_url = NULL;
    segments[2].url = "file:///middle.ts";
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].url = "HTTP://test/c.ts";
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].url = NULL;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].url = "https://test/c.ts";
    if (!ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    results[3].audio_timing.min_sample_duration = 0;
    if (ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    results[3] = timed_result(9400000);
    results[0].audio_timing.min_sample_duration = 0;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[0] = timed_result(100000000);
    results[4].audio_timing.first_pts += 20000;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[4] = timed_result(104000000);
    results[3].video_timing.last_pts -= 80000;
    if (ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    results[3] = timed_result(9400000);
    segments[2].duration += 1000000;
    if (ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    segments[2].duration = 4000000;
    segments[2].discontinuity = 1;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    segments[2].discontinuity = 0;
    results[1].width = 1280;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[1] = timed_result(1400000);
    results[1].frame_rate = 30;
    results[1].video_timing.last_pts += 6667;
    results[1].video_timing.min_sample_duration = 1000000.0 / 30;
    results[1].video_timing.max_sample_duration = 1000000.0 / 30;
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[1] = timed_result(1400000);

    /* A continuous program and independently reset segments are not insertions. */
    results[1] = timed_result(104000000);
    results[3] = timed_result(112000000);
    results[4] = timed_result(116000000);
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[0] = results[1] = results[3] = results[4] = timed_result(1400000);
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;

    /* Overlapping clocks cannot establish a separate inserted timeline. */
    results[0] = timed_result(100000000);
    results[1] = timed_result(98000000);
    results[3] = timed_result(106000000);
    results[4] = timed_result(104000000);
    if (ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4))
        return 1;
    results[1] = timed_result(90000000);
    results[3] = timed_result(98000000);
    if (!ff_hls_ad_timeline_boundaries(segments, results, 5, 1, 4) ||
        ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;

    /* The same seam survives MPEG-TS wrap and FFmpeg's negative wrap correction. */
    results[0] = timed_result(95443717689LL - 2000000);
    results[1] = timed_result(100000000);
    results[3] = timed_result(108000000);
    results[4] = timed_result(2000000);
    if (!ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    results[0] = timed_result(-2000000);
    if (!ff_hls_ad_confirm_timeline_window(segments, results, 5, 1, 4))
        return 1;
    return 0;
}

static int check_variable_timeline(void)
{
    FFHLSAdSegment segments[9] = {0};
    FFHLSAdProbeResult results[9] = {0};
    FFHLSAdProbeResult after;
    const int64_t offsets[] = {0, 1000, 1001, -41000, -41001};

    for (int i = 0; i < 9; i++) {
        segments[i] = (FFHLSAdSegment) {
            .url = "http://test/segment.ts", .size = -1,
            .duration = i == 7 ? 1760000 : 4000000,
            .discontinuity = i == 1 || i == 8,
        };
        results[i] = timed_result(1400000 + (i - 1) * 4000000LL);
    }
    results[0] = timed_result(100000000);
    results[0].frame_rate = 24.4;
    results[0].frame_rate_margin = 0.6;
    results[0].video_timing.last_pts -= 40000;
    results[0].video_timing.max_sample_duration = 80000;
    results[7].video_timing.last_pts -= 2240000;
    results[7].audio_timing.last_pts -= 2240000;
    results[8] = timed_result(104000000);
    results[8].frame_rate = 23.97;
    results[8].frame_rate_margin = 0.4;
    results[8].video_timing.max_sample_duration = 80000;
    after = results[8];
    /* The point rates need not match when both original clocks resume. */
    if (ff_hls_ad_candidate_start(segments, results, 9, 1) ||
        !ff_hls_ad_confirm_timeline_window(segments, results, 9, 1, 8))
        return 1;
    results[8].frame_rate = 22;
    if (!ff_hls_ad_confirm_timeline_window(segments, results, 9, 1, 8))
        return 1;
    for (unsigned int i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
        int expected = offsets[i] >= -41000 && offsets[i] <= 1000;
        results[8] = after;
        results[8].video_timing.first_pts += offsets[i];
        results[8].video_timing.last_pts += offsets[i];
        if (!!ff_hls_ad_confirm_timeline_window(segments, results, 9, 1, 8) != expected)
            return 1;
    }
    results[8] = after;
    results[8].audio_timing.first_pts += 2000;
    if (ff_hls_ad_timeline_boundaries(segments, results, 9, 1, 8))
        return 1;
    results[8] = after;
    results[0].video_timing.max_sample_duration = 39999;
    if (ff_hls_ad_timeline_boundaries(segments, results, 9, 1, 8))
        return 1;
    results[0].video_timing.max_sample_duration = 80000;
    results[7].video_timing.last_pts -= 80000;
    return ff_hls_ad_confirm_timeline_window(segments, results, 9, 1, 8) != 0;
}

static int check_candidates(void)
{
    FFHLSAdSegment segments[40] = {0};
    int count = 8;
    FFHLSAdCandidate candidates[40];

    segments[2].discontinuity = segments[6].discontinuity = 1;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 1 ||
        candidates[0].first != 2 || candidates[0].end != 6)
        return 1;
    /* Only bounded interior blocks are probed, shortest first. */
    segments[5].discontinuity = 1;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 2 ||
        candidates[0].first != 5 || candidates[1].first != 2)
        return 1;
    segments[5].discontinuity = 0;
    /* A leading EXT-X-DISCONTINUITY does not create an empty block. */
    segments[0].discontinuity = 1;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 1 ||
        candidates[0].first != 2)
        return 1;
    count = 3;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 0)
        return 1;
    count = 8;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 1 ||
        candidates[0].first != 2)
        return 1;
    segments[2].discontinuity = 0;
    segments[1].discontinuity = 1;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 1 ||
        candidates[0].first != 1)
        return 1;
    for (int i = 0; i < 40; i++) {
        segments[i].discontinuity = 0;
    }
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 0)
        return 1;
    /* An opening block is never a media-probe candidate, regardless of size. */
    count = 33;
    segments[30].discontinuity = 1;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 0)
        return 1;
    count = 32;
    if (ff_hls_ad_find_candidates(segments, count, candidates) != 0)
        return 1;
    return 0;
}

static int check_no_leading_window(void)
{
    FFHLSAdSegment segments[6] = {0};
    FFHLSAdProbeResult results[6];
    const int starts[] = {0, -1, INT_MIN};

    for (int i = 0; i < 6; i++) {
        segments[i].url = "http://test/segment.ts";
        segments[i].duration = 4000000;
        segments[i].size = -1;
        results[i] = timed_result(1400000);
        if (i >= 2) {
            results[i].width = 1280;
            results[i].height = 648;
        }
    }
    segments[2].discontinuity = 1;
    for (int i = 0; i < 3; i++) {
        int first = starts[i];
        if (ff_hls_ad_candidate_start(segments, results, 6, first) ||
            ff_hls_ad_candidate_boundaries(segments, results, 6, first, 2) ||
            ff_hls_ad_confirm_window(segments, results, 6, first, 2) ||
            ff_hls_ad_confirm_timeline_window(segments, results, 6, first, 2))
            return 1;
    }
    return 0;
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

    if (check_external_opening()) {
        fprintf(stderr, "External HLS opening block mismatch\n");
        return 1;
    }
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

    if (check_candidates() || check_no_leading_window() || check_timeline() ||
        check_variable_timeline() ||
        check(cue, 3, 1, 1, 1, 1, cue_mask) ||
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
