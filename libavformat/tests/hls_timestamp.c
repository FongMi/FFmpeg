/*
 * Copyright (c) 2026 FongMi
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <inttypes.h>
#include <stdio.h>

#include "libavformat/hls_timestamp.h"

/* Microsecond timestamps sampled at the two splice boundaries of the
 * regression stream. */
static const int64_t discontinuity_threshold = 16 * AV_TIME_BASE;
static const int64_t video_frame_duration = 40000;
static const int64_t audio_frame_duration = 23220;

static int check_timestamp(const char *name, int64_t actual, int64_t expected)
{
    if (actual == expected)
        return 0;

    fprintf(stderr, "%s: expected %"PRId64", got %"PRId64"\n",
            name, expected, actual);
    return 1;
}

static void init_states(FFHLSTimestampState *state,
                        FFHLSTimestampStreamState *video,
                        FFHLSTimestampStreamState *audio)
{
    ff_hls_timestamp_init(state);
    ff_hls_timestamp_stream_init(video);
    ff_hls_timestamp_stream_init(audio);
    video->baseline = 0;
    audio->baseline = 33378;
}

static int test_ad_start(void)
{
    FFHLSTimestampState state;
    FFHLSTimestampStreamState video;
    FFHLSTimestampStreamState audio;

    init_states(&state, &video, &audio);
    if (check_timestamp("video before ad",
                        ff_hls_timestamp_map(&state, &video, 298520000,
                                             video_frame_duration,
                                             discontinuity_threshold),
                        298520000) ||
        check_timestamp("audio before ad",
                        ff_hls_timestamp_map(&state, &audio, 298556300,
                                             audio_frame_duration,
                                             discontinuity_threshold),
                        298556300) ||
        check_timestamp("video ad reset",
                        ff_hls_timestamp_map(&state, &video, 1400000,
                                             video_frame_duration,
                                             discontinuity_threshold),
                        298560000) ||
        check_timestamp("audio after ad reset",
                        ff_hls_timestamp_map(&state, &audio, 1433778,
                                             audio_frame_duration,
                                             discontinuity_threshold),
                        298593778) ||
        check_timestamp("ad reset offset", state.offset, 297160000))
        return 1;

    return 0;
}

static int test_ad_end(void)
{
    FFHLSTimestampState state;
    FFHLSTimestampStreamState video;
    FFHLSTimestampStreamState audio;

    init_states(&state, &video, &audio);
    state.offset = 297160000;
    state.last_dts = 324600178;

    video.last_dts = 324520000;
    video.last_duration = video_frame_duration;
    audio.last_dts = 324600178;
    audio.last_duration = audio_frame_duration;
    if (check_timestamp("video main resume",
                        ff_hls_timestamp_map(&state, &video, 298560000,
                                             video_frame_duration,
                                             discontinuity_threshold),
                        324560000) ||
        check_timestamp("audio main resume overlap",
                        ff_hls_timestamp_map(&state, &audio, 298579522,
                                             audio_frame_duration,
                                             discontinuity_threshold),
                        324600178) ||
        check_timestamp("main resume offset", state.offset, 26000000))
        return 1;

    return 0;
}

static int test_seek_reanchor(void)
{
    FFHLSTimestampState state;
    FFHLSTimestampStreamState video;

    ff_hls_timestamp_init(&state);
    ff_hls_timestamp_stream_init(&video);
    video.baseline = 0;
    ff_hls_timestamp_reset(&state, &video, 1, 324560000);
    return check_timestamp("seek segment reanchor",
                           ff_hls_timestamp_map(&state, &video, 298560000,
                                                video_frame_duration,
                                                discontinuity_threshold),
                           324560000);
}

static int test_interleaved_stream_gap(void)
{
    FFHLSTimestampState state;
    FFHLSTimestampStreamState video;

    ff_hls_timestamp_init(&state);
    ff_hls_timestamp_stream_init(&video);
    state.last_dts = 119000000;
    video.last_dts = 100000000;
    video.last_duration = video_frame_duration;
    if (check_timestamp("interleaved stream gap",
                        ff_hls_timestamp_map(&state, &video, 120000000,
                                             video_frame_duration,
                                             discontinuity_threshold),
                        120000000) ||
        check_timestamp("interleaved stream offset", state.offset, 0))
        return 1;

    return 0;
}

int main(void)
{
    return test_ad_start() ||
           test_ad_end() ||
           test_seek_reanchor() ||
           test_interleaved_stream_gap();
}
