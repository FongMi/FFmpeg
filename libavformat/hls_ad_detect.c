/*
 * HLS ad break detection from parsed media-playlist metadata.
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

#include <string.h>

#include "hls_ad_detect.h"

static int equal_string(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static int equal_segment(const FFHLSAdSegment *a, const FFHLSAdSegment *b)
{
    return equal_string(a->url, b->url) &&
           equal_string(a->key, b->key) &&
           equal_string(a->init_url, b->init_url) &&
           a->duration == b->duration && a->offset == b->offset &&
           a->size == b->size && a->key_type == b->key_type &&
           (!a->key_type || !memcmp(a->iv, b->iv, sizeof(a->iv)));
}

static int equal_block(const FFHLSAdSegment *segments,
                       int first, int end, int other_first, int other_end)
{
    if (end - first != other_end - other_first)
        return 0;
    for (int i = 0; i < end - first; i++)
        if (!equal_segment(&segments[first + i], &segments[other_first + i]))
            return 0;
    return 1;
}

static void mark_matching_edge(const FFHLSAdSegment *segments, int count,
                               int edge_first, int edge_end, uint8_t *remove)
{
    int matches = 0;
    int first = 0;

    /* An isolated repeated segment is not evidence of an ad break. */
    if (edge_end - edge_first < 2)
        return;
    for (int end = 1; end <= count; end++) {
        if (end < count && !segments[end].discontinuity)
            continue;
        if (equal_block(segments, edge_first, edge_end, first, end))
            matches++;
        first = end;
    }
    if (matches < 2)
        return;
    first = 0;
    for (int end = 1; end <= count; end++) {
        if (end < count && !segments[end].discontinuity)
            continue;
        if (equal_block(segments, edge_first, edge_end, first, end))
            memset(remove + first, 1, end - first);
        first = end;
    }
}

int ff_hls_ad_detect(const FFHLSAdSegment *segments, int count,
                     int has_cue, int valid_cue, int allow_repeated_blocks,
                     uint8_t *remove)
{
    int removed = 0;
    int last_block = 0;

    if (!segments || !remove || count <= 0)
        return 0;
    memset(remove, 0, count);
    if (has_cue) {
        if (!valid_cue)
            return 0;
        for (int i = 0; i < count; i++)
            remove[i] = !!segments[i].cue_ad;
    } else if (allow_repeated_blocks) {
        int first_end = count;
        for (int i = 1; i < count; i++) {
            if (segments[i].discontinuity) {
                if (first_end == count)
                    first_end = i;
                last_block = i;
            }
        }
        if (first_end < count) {
            mark_matching_edge(segments, count, 0, first_end, remove);
            mark_matching_edge(segments, count, last_block, count, remove);
        }
    }
    for (int i = 0; i < count; i++)
        removed += remove[i];
    if (removed == count) {
        memset(remove, 0, count);
        return 0;
    }
    return removed;
}

enum Difference {
    DIFFERENCE_NONE,
    DIFFERENCE_RESOLUTION,
    DIFFERENCE_FRAME_RATE,
    DIFFERENCE_HIGH_BITRATE,
    DIFFERENCE_LOW_BITRATE,
};

static int same_frame_rate(const FFHLSAdProbeResult *a,
                           const FFHLSAdProbeResult *b)
{
    double minimum = a->frame_rate < b->frame_rate ? a->frame_rate : b->frame_rate;
    double delta = a->frame_rate > b->frame_rate ?
                   a->frame_rate - b->frame_rate : b->frame_rate - a->frame_rate;
    return minimum > 0 && delta <= minimum * 0.02;
}

static int same_resolution(const FFHLSAdProbeResult *a,
                           const FFHLSAdProbeResult *b)
{
    return a->width == b->width && a->height == b->height;
}

static double bitrate(const FFHLSAdSegment *segment,
                      const FFHLSAdProbeResult *result)
{
    return (double)result->size / segment->duration;
}

static enum Difference difference(const FFHLSAdSegment *a_segment,
                                  const FFHLSAdProbeResult *a,
                                  const FFHLSAdSegment *b_segment,
                                  const FFHLSAdProbeResult *b)
{
    double a_low, a_high, b_low, b_high;
    double a_bitrate, b_bitrate;

    if (!same_resolution(a, b))
        return DIFFERENCE_RESOLUTION;
    a_low = a->frame_rate - a->frame_rate_margin;
    a_high = a->frame_rate + a->frame_rate_margin;
    b_low = b->frame_rate - b->frame_rate_margin;
    b_high = b->frame_rate + b->frame_rate_margin;
    if (a_low > b_high || b_low > a_high)
        return DIFFERENCE_FRAME_RATE;
    if (!same_frame_rate(a, b))
        return DIFFERENCE_NONE;
    a_bitrate = bitrate(a_segment, a);
    b_bitrate = bitrate(b_segment, b);
    if (b_bitrate > a_bitrate * 2)
        return DIFFERENCE_HIGH_BITRATE;
    if (b_bitrate < a_bitrate * 0.5)
        return DIFFERENCE_LOW_BITRATE;
    return DIFFERENCE_NONE;
}

static int valid_measurement(const FFHLSAdSegment *segment,
                             const FFHLSAdProbeResult *result)
{
    return segment->duration > 0 && result->size > 0 &&
           result->width > 0 && result->height > 0 &&
           result->frame_rate > 0 && result->frame_rate_margin >= 0;
}

int ff_hls_ad_candidate_start(const FFHLSAdSegment *segments,
                              const FFHLSAdProbeResult *results,
                              int count, int first)
{
    return segments && results && first >= 1 && first < count &&
           valid_measurement(&segments[first - 1], &results[first - 1]) &&
           valid_measurement(&segments[first], &results[first]) &&
           difference(&segments[first - 1], &results[first - 1],
                      &segments[first], &results[first]) != DIFFERENCE_NONE;
}

int ff_hls_ad_candidate_boundaries(const FFHLSAdSegment *segments,
                                   const FFHLSAdProbeResult *results,
                                   int count, int first, int end)
{
    const FFHLSAdProbeResult *before, *inside, *after;
    enum Difference kind;
    double before_bitrate, after_bitrate;

    if (!ff_hls_ad_candidate_start(segments, results, count, first) ||
        end <= first || end >= count || end - first > 30 ||
        segments[end].url == NULL ||
        !valid_measurement(&segments[end], &results[end]))
        return 0;
    before = &results[first - 1];
    inside = &results[first];
    after = &results[end];
    kind = difference(&segments[first - 1], before, &segments[first], inside);
    if (kind == DIFFERENCE_NONE || !same_resolution(before, after) ||
        (kind != DIFFERENCE_RESOLUTION && !same_frame_rate(before, after)))
        return 0;
    if (kind == DIFFERENCE_HIGH_BITRATE || kind == DIFFERENCE_LOW_BITRATE) {
        if (end - first < 2)
            return 0;
        before_bitrate = bitrate(&segments[first - 1], before);
        after_bitrate = bitrate(&segments[end], after);
        if (before_bitrate > after_bitrate * 1.25 ||
            after_bitrate > before_bitrate * 1.25)
            return 0;
    }
    if (difference(&segments[end], after, &segments[first], inside) != kind)
        return 0;
    return 1;
}

int ff_hls_ad_confirm_window(const FFHLSAdSegment *segments,
                             const FFHLSAdProbeResult *results,
                             int count, int first, int end)
{
    const FFHLSAdProbeResult *before, *inside, *after;
    enum Difference kind;

    if (!ff_hls_ad_candidate_boundaries(segments, results, count, first, end))
        return 0;
    before = &results[first - 1];
    inside = &results[first];
    after = &results[end];
    kind = difference(&segments[first - 1], before, &segments[first], inside);
    for (int i = first; i < end; i++) {
        if (!valid_measurement(&segments[i], &results[i]))
            return 0;
        if (!same_resolution(inside, &results[i]) ||
            (kind != DIFFERENCE_RESOLUTION && !same_frame_rate(inside, &results[i])) ||
            difference(&segments[first - 1], before, &segments[i], &results[i]) != kind ||
            difference(&segments[end], after, &segments[i], &results[i]) != kind)
            return 0;
    }
    return 1;
}

static int valid_timing(const FFHLSAdTiming *timing)
{
    double span = (double)timing->last_pts - timing->first_pts;

    return span > 0 && span < HLS_AD_PTS_WRAP_US / 2 &&
           timing->sample_duration > 0 && timing->sample_duration <= span;
}

static int64_t timestamp_delta(int64_t first, int64_t second)
{
    int64_t delta = (first % HLS_AD_PTS_WRAP_US - second % HLS_AD_PTS_WRAP_US) %
                    HLS_AD_PTS_WRAP_US;

    if (delta > HLS_AD_PTS_WRAP_US / 2)
        delta -= HLS_AD_PTS_WRAP_US;
    else if (delta < -HLS_AD_PTS_WRAP_US / 2)
        delta += HLS_AD_PTS_WRAP_US;
    return delta;
}

static int same_cadence(const FFHLSAdTiming *a, const FFHLSAdTiming *b)
{
    double delta = a->sample_duration - b->sample_duration;

    return delta >= -HLS_AD_TIMESTAMP_TOLERANCE_US &&
           delta <= HLS_AD_TIMESTAMP_TOLERANCE_US;
}

static int timeline_resumes(const FFHLSAdTiming *before,
                            const FFHLSAdTiming *after)
{
    double gap;

    if (!valid_timing(before) || !valid_timing(after) ||
        !same_cadence(before, after))
        return 0;
    gap = timestamp_delta(after->first_pts, before->last_pts) -
          before->sample_duration;
    return gap >= -HLS_AD_TIMESTAMP_TOLERANCE_US &&
           gap <= HLS_AD_TIMESTAMP_TOLERANCE_US;
}

static int separate_timeline(const FFHLSAdTiming *before,
                             const FFHLSAdTiming *after,
                             const FFHLSAdTiming *head,
                             const FFHLSAdTiming *tail)
{
    double retained_span, candidate_start, candidate_span;

    if (!valid_timing(head) || !valid_timing(tail))
        return 0;
    retained_span = (double)before->last_pts - before->first_pts +
                    timestamp_delta(after->first_pts, before->last_pts) +
                    ((double)after->last_pts - after->first_pts) +
                    after->sample_duration;
    candidate_span = timestamp_delta(tail->last_pts, head->first_pts) +
                     tail->sample_duration;
    if (retained_span <= 0 || retained_span >= HLS_AD_PTS_WRAP_US / 2 ||
        candidate_span <= 0 || candidate_span >= HLS_AD_PTS_WRAP_US / 2)
        return 0;
    candidate_start = timestamp_delta(head->first_pts, before->first_pts);
    return candidate_start + candidate_span <= -HLS_AD_TIMESTAMP_TOLERANCE_US ||
           candidate_start >= retained_span + HLS_AD_TIMESTAMP_TOLERANCE_US;
}

int ff_hls_ad_timeline_boundaries(const FFHLSAdSegment *segments,
                                 const FFHLSAdProbeResult *results,
                                 int count, int first, int end)
{
    const FFHLSAdProbeResult *before, *inside, *after;

    if (!segments || !results || first < 1 || end <= first ||
        end >= count || end - first > 30 ||
        !segments[first].discontinuity || !segments[end].discontinuity ||
        !valid_measurement(&segments[first - 1], &results[first - 1]) ||
        !valid_measurement(&segments[first], &results[first]) ||
        !valid_measurement(&segments[end], &results[end]))
        return 0;
    for (int i = first; i < end; i++)
        if ((i > first && segments[i].discontinuity) || segments[i].key_type ||
            segments[i].size >= 0 || segments[i].init_url ||
            segments[i].duration <= 0 || !segments[i].url ||
            (strncmp(segments[i].url, "http://", 7) &&
             strncmp(segments[i].url, "https://", 8)))
            return 0;
    before = &results[first - 1];
    inside = &results[first];
    after = &results[end];
    return difference(&segments[first - 1], before, &segments[first], inside) ==
               DIFFERENCE_NONE &&
           same_resolution(before, after) && same_frame_rate(before, after) &&
           timeline_resumes(&before->video_timing, &after->video_timing) &&
           timeline_resumes(&before->audio_timing, &after->audio_timing) &&
           separate_timeline(&before->video_timing, &after->video_timing,
                             &inside->video_timing, &inside->video_timing) &&
           separate_timeline(&before->audio_timing, &after->audio_timing,
                             &inside->audio_timing, &inside->audio_timing);
}

static int timeline_duration(const FFHLSAdTiming *head,
                             const FFHLSAdTiming *tail, double duration,
                             int segment_count)
{
    double span, margin, delta;

    if (!valid_timing(head) || !valid_timing(tail) || !same_cadence(head, tail))
        return 0;
    if (segment_count > 1 &&
        timestamp_delta(tail->first_pts, head->last_pts) <
            head->sample_duration - HLS_AD_TIMESTAMP_TOLERANCE_US)
        return 0;
    span = timestamp_delta(tail->last_pts, head->first_pts) +
           tail->sample_duration;
    margin = (head->sample_duration > tail->sample_duration ?
               head->sample_duration : tail->sample_duration) +
              HLS_AD_TIMESTAMP_TOLERANCE_US;
    delta = span - duration;
    return span > 0 && span < HLS_AD_PTS_WRAP_US / 2 &&
           delta >= -margin && delta <= margin;
}

int ff_hls_ad_confirm_timeline_window(const FFHLSAdSegment *segments,
                                     const FFHLSAdProbeResult *results,
                                     int count, int first, int end)
{
    const FFHLSAdProbeResult *before, *head, *tail, *after;
    double duration = 0;

    if (!ff_hls_ad_timeline_boundaries(segments, results, count, first, end) ||
        !valid_measurement(&segments[end - 1], &results[end - 1]))
        return 0;
    for (int i = first; i < end; i++)
        duration += segments[i].duration;
    before = &results[first - 1];
    head = &results[first];
    tail = &results[end - 1];
    after = &results[end];
    return timeline_duration(&head->video_timing, &tail->video_timing, duration,
                              end - first) &&
           timeline_duration(&head->audio_timing, &tail->audio_timing, duration,
                              end - first) &&
           separate_timeline(&before->video_timing, &after->video_timing,
                             &head->video_timing, &tail->video_timing) &&
           separate_timeline(&before->audio_timing, &after->audio_timing,
                             &head->audio_timing, &tail->audio_timing);
}

int ff_hls_ad_same_content(const FFHLSAdSegment *segments,
                           const FFHLSAdProbeResult *results, int count,
                           int first, int end, int other_first, int other_end)
{
    if (!segments || !results || first < 0 || other_first < 0 ||
        end <= first || other_end <= other_first ||
        end > count || other_end > count || end - first < 2 ||
        end - first != other_end - other_first)
        return 0;
    for (int i = 0; i < end - first; i++) {
        int a = first + i, b = other_first + i;
        if (segments[a].duration != segments[b].duration ||
            !valid_measurement(&segments[a], &results[a]) ||
            !valid_measurement(&segments[b], &results[b]) ||
            !results[a].has_content_fingerprint ||
            !results[b].has_content_fingerprint ||
            memcmp(results[a].content_fingerprint,
                   results[b].content_fingerprint,
                   sizeof(results[a].content_fingerprint)))
            return 0;
    }
    return 1;
}
