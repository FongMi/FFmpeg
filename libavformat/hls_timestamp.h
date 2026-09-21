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

#ifndef AVFORMAT_HLS_TIMESTAMP_H
#define AVFORMAT_HLS_TIMESTAMP_H

#include "libavutil/avutil.h"

typedef struct FFHLSTimestampState {
    int64_t offset;
    int64_t segment_start;
    int64_t last_dts;
} FFHLSTimestampState;

typedef struct FFHLSTimestampStreamState {
    int64_t baseline;
    int64_t last_dts;
    int64_t last_duration;
} FFHLSTimestampStreamState;

void ff_hls_timestamp_init(FFHLSTimestampState *state);
void ff_hls_timestamp_stream_init(FFHLSTimestampStreamState *state);
void ff_hls_timestamp_reset(FFHLSTimestampState *state,
                            FFHLSTimestampStreamState *streams,
                            int nb_streams, int64_t segment_start);
int64_t ff_hls_timestamp_map(FFHLSTimestampState *state,
                             FFHLSTimestampStreamState *stream,
                             int64_t dts, int64_t duration,
                             int64_t discontinuity_threshold);

#endif /* AVFORMAT_HLS_TIMESTAMP_H */
