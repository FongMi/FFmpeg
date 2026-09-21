/*
 * HLS timestamp discontinuity mapping
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

#include "libavutil/common.h"

#include "hls_timestamp.h"

static int is_timestamp_discontinuous(int64_t timestamp, int64_t reference,
                                      int64_t threshold)
{
    return timestamp < av_sat_sub64(reference, threshold) ||
           timestamp > av_sat_add64(reference, threshold);
}

void ff_hls_timestamp_init(FFHLSTimestampState *state)
{
    state->offset        = 0;
    state->segment_start = AV_NOPTS_VALUE;
    state->last_dts      = AV_NOPTS_VALUE;
}

void ff_hls_timestamp_stream_init(FFHLSTimestampStreamState *state)
{
    state->baseline      = AV_NOPTS_VALUE;
    state->last_dts      = AV_NOPTS_VALUE;
    state->last_duration = 0;
}

void ff_hls_timestamp_reset(FFHLSTimestampState *state,
                            FFHLSTimestampStreamState *streams,
                            int nb_streams, int64_t segment_start)
{
    state->offset        = 0;
    state->segment_start = segment_start;
    state->last_dts      = AV_NOPTS_VALUE;

    for (int i = 0; i < nb_streams; i++) {
        streams[i].last_dts      = AV_NOPTS_VALUE;
        streams[i].last_duration = 0;
    }
}

int64_t ff_hls_timestamp_map(FFHLSTimestampState *state,
                             FFHLSTimestampStreamState *stream,
                             int64_t dts, int64_t duration,
                             int64_t discontinuity_threshold)
{
    int64_t expected;
    int64_t mapped;

    if (dts == AV_NOPTS_VALUE)
        return dts;

    if (state->segment_start != AV_NOPTS_VALUE) {
        int64_t baseline = stream->baseline == AV_NOPTS_VALUE ? 0 : stream->baseline;
        state->offset = av_sat_sub64(av_sat_add64(state->segment_start, baseline), dts);
        state->segment_start = AV_NOPTS_VALUE;
    }

    mapped = av_sat_add64(dts, state->offset);
    if (stream->last_dts != AV_NOPTS_VALUE) {
        expected = av_sat_add64(stream->last_dts, FFMAX(stream->last_duration, 0));
        /* A stream can legitimately be absent while another stream advances.
         * Rebuild the shared offset only when the timestamp is also outside
         * the playlist-wide range. */
        if (is_timestamp_discontinuous(mapped, expected, discontinuity_threshold) &&
            (state->last_dts == AV_NOPTS_VALUE ||
             is_timestamp_discontinuous(mapped, state->last_dts,
                                        discontinuity_threshold))) {
            state->offset = av_sat_add64(state->offset, av_sat_sub64(expected, mapped));
            mapped = expected;
        }
        mapped = FFMAX(mapped, stream->last_dts);
    }

    stream->last_dts      = mapped;
    stream->last_duration = duration;
    if (state->last_dts == AV_NOPTS_VALUE || mapped > state->last_dts)
        state->last_dts = mapped;
    return mapped;
}
