/*
 * Bounded MPEG-TS measurements for unmarked HLS ad candidates.
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
#ifndef AVFORMAT_HLS_AD_PROBE_H
#define AVFORMAT_HLS_AD_PROBE_H

#include "avformat.h"
#include "hls_ad_detect.h"

#define HLS_AD_PROBE_MAX_SEGMENT_BYTES (8 * 1024 * 1024)

int ff_hls_ad_probe(const char *url, const AVDictionary *avio_opts,
                    const AVIOInterruptCB *interrupt_callback,
                    const char *protocol_whitelist, const char *protocol_blacklist,
                    int64_t deadline_us, FFHLSAdProbeResult *result);

#endif
