/*
 * TTML timing
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_TTML_TIMING_H
#define AVCODEC_TTML_TIMING_H

#include <stdint.h>

#include <libxml/tree.h>

#include "libavutil/rational.h"

#define TTML_NS       "http://www.w3.org/ns/ttml"
#define TTML_PARAM_NS "http://www.w3.org/ns/ttml#parameter"
#define TTML_MAX_PACKET_SIZE (4 * 1024 * 1024)
#define TTML_MAX_TREE_DEPTH  64

typedef struct TTMLTiming {
    AVRational frame_rate;
    AVRational tick_rate;
    int nominal_frame_rate;
    int sub_frame_rate;
} TTMLTiming;

typedef struct TTMLInterval {
    int64_t begin;
    int64_t end;
} TTMLInterval;

int avpriv_ttml_parse_timing_parameters(xmlNode *root, TTMLTiming *timing);
int64_t avpriv_ttml_parse_time_expression(const xmlChar *value,
                                         const TTMLTiming *timing);
int avpriv_ttml_resolve_interval(xmlNode *node, const TTMLTiming *timing,
                                 int64_t root_end, TTMLInterval *interval,
                                 int depth);
int avpriv_ttml_walk_paragraphs(xmlNode *root, const TTMLTiming *timing,
                                int (*visit)(void *, xmlNode *,
                                             const TTMLInterval *),
                                void *opaque);

#endif /* AVCODEC_TTML_TIMING_H */
