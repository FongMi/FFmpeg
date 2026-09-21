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

#include <stdlib.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"
#include "avformat.h"
#include "avio_internal.h"
#include "hls_ad_probe.h"

#define MAX_SAMPLES 8192
#define MIN_SAMPLES 6
#define PTS_PRECISION_US (1000000.0 / 90000 + 2)

typedef struct HLSAdProbeInterrupt {
    const AVIOInterruptCB *outer;
    int64_t deadline_us;
} HLSAdProbeInterrupt;

typedef struct HLSAdMemory {
    const uint8_t *data;
    int size;
    int pos;
} HLSAdMemory;

static int probe_interrupted(void *opaque)
{
    HLSAdProbeInterrupt *state = opaque;
    return (state->outer && state->outer->callback &&
            state->outer->callback(state->outer->opaque)) ||
           av_gettime_relative() >= state->deadline_us;
}

static int compare_pts(const void *a, const void *b)
{
    int64_t first = *(const int64_t *)a;
    int64_t second = *(const int64_t *)b;
    return (first > second) - (first < second);
}

static int read_memory(void *opaque, uint8_t *buffer, int size)
{
    HLSAdMemory *source = opaque;
    int available = source->size - source->pos;

    if (!available)
        return AVERROR_EOF;
    size = FFMIN(size, available);
    memcpy(buffer, source->data + source->pos, size);
    source->pos += size;
    return size;
}

static int64_t seek_memory(void *opaque, int64_t offset, int whence)
{
    HLSAdMemory *source = opaque;
    int64_t pos;

    if (whence == AVSEEK_SIZE)
        return source->size;
    if (whence == SEEK_SET)
        pos = offset;
    else if (whence == SEEK_CUR)
        pos = source->pos + offset;
    else if (whence == SEEK_END)
        pos = source->size + offset;
    else
        return AVERROR(EINVAL);
    if (pos < 0 || pos > source->size)
        return AVERROR(EINVAL);
    source->pos = pos;
    return pos;
}

int ff_hls_ad_probe(const char *url, const AVDictionary *avio_opts,
                    const AVIOInterruptCB *interrupt_callback,
                    const char *protocol_whitelist, const char *protocol_blacklist,
                    int64_t deadline_us, FFHLSAdProbeResult *result)
{
    AVFormatContext *input = NULL;
    AVIOContext *network = NULL, *memory_io = NULL;
    AVDictionary *options = NULL;
    AVPacket *packet = NULL;
    struct AVSHA *sha = NULL;
    int64_t *pts = NULL;
    uint8_t *data = NULL, *io_buffer = NULL;
    HLSAdMemory memory = {0};
    HLSAdProbeInterrupt interrupt = {interrupt_callback, deadline_us};
    int video = -1, count = 0, ret = AVERROR_INVALIDDATA;
    int size = 0;
    double span, mean, deviation = 0;

    if (!url || !result ||
        (!av_strstart(url, "http://", NULL) &&
         !av_strstart(url, "https://", NULL)))
        return AVERROR(EINVAL);
    if (probe_interrupted(&interrupt))
        return AVERROR_EXIT;
    packet = av_packet_alloc();
    pts = av_malloc_array(MAX_SAMPLES, sizeof(*pts));
    data = av_malloc(HLS_AD_PROBE_MAX_SEGMENT_BYTES + 1);
    io_buffer = av_malloc(32768);
    if (!packet || !pts || !data || !io_buffer) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    av_dict_copy(&options, avio_opts, 0);
    ret = ffio_open_whitelist(&network, url, AVIO_FLAG_READ,
                              &(AVIOInterruptCB){probe_interrupted, &interrupt},
                              &options, protocol_whitelist, protocol_blacklist);
    if (ret < 0)
        goto cleanup;
    while (size <= HLS_AD_PROBE_MAX_SEGMENT_BYTES) {
        int read = avio_read(network, data + size,
                             FFMIN(32768,
                                   HLS_AD_PROBE_MAX_SEGMENT_BYTES + 1 - size));
        if (read == AVERROR_EOF || read == 0)
            break;
        if (read < 0) {
            ret = read;
            goto cleanup;
        }
        size += read;
        if (probe_interrupted(&interrupt)) {
            ret = AVERROR_EXIT;
            goto cleanup;
        }
    }
    if (size <= 0 || size > HLS_AD_PROBE_MAX_SEGMENT_BYTES) {
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }
    avio_closep(&network);
    memory = (HLSAdMemory){data, size, 0};
    memory_io = avio_alloc_context(io_buffer, 32768, 0, &memory,
                                    read_memory, NULL, seek_memory);
    if (!memory_io) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    io_buffer = NULL;
    memory_io->seekable = AVIO_SEEKABLE_NORMAL;
    input = avformat_alloc_context();
    if (!input) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    input->pb = memory_io;
    input->flags |= AVFMT_FLAG_CUSTOM_IO;
    input->probesize = 1024 * 1024;
    input->max_analyze_duration = 2 * AV_TIME_BASE;
    ret = avformat_open_input(&input, NULL, av_find_input_format("mpegts"), NULL);
    if (ret < 0)
        goto cleanup;
    ret = avformat_find_stream_info(input, NULL);
    if (ret < 0)
        goto cleanup;
    for (unsigned int i = 0; i < input->nb_streams; i++) {
        if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (video >= 0 || input->streams[i]->codecpar->codec_id != AV_CODEC_ID_H264) {
                ret = AVERROR_INVALIDDATA;
                goto cleanup;
            }
            video = i;
        }
    }
    if (video < 0 || input->streams[video]->codecpar->width <= 0 ||
        input->streams[video]->codecpar->height <= 0) {
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }
    while (count < MAX_SAMPLES && (ret = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == video && packet->pts != AV_NOPTS_VALUE)
            pts[count++] = av_rescale_q(packet->pts,
                                         input->streams[video]->time_base,
                                         (AVRational){1, AV_TIME_BASE});
        av_packet_unref(packet);
        if (probe_interrupted(&interrupt)) {
            ret = AVERROR_EXIT;
            goto cleanup;
        }
    }
    av_log(input, AV_LOG_DEBUG, "HLS ad probe: read=%d, samples=%d, bytes=%d\n",
           ret, count, size);
    if (ret != AVERROR_EOF || count < MIN_SAMPLES) {
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }
    qsort(pts, count, sizeof(*pts), compare_pts);
    span = (double)pts[count - 1] - pts[0];
    if (span <= PTS_PRECISION_US) {
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }
    mean = span / (count - 1);
    for (int i = 1; i < count; i++) {
        double drift = ((double)pts[i] - pts[0]) - i * mean;
        if (pts[i] <= pts[i - 1] ||
            drift > mean + 2 || drift < -mean - 2) {
            ret = AVERROR_INVALIDDATA;
            goto cleanup;
        }
        deviation = FFMAX(deviation, drift < 0 ? -drift : drift);
    }
    *result = (FFHLSAdProbeResult) {
        .size = size,
        .frame_rate = (count - 1) * 1000000.0 / span,
        .frame_rate_margin =
            ((count - 1) * 1000000.0 / span) * PTS_PRECISION_US /
            (span - PTS_PRECISION_US) +
            (deviation > PTS_PRECISION_US ? 1000000.0 / span : 0),
        .width = input->streams[video]->codecpar->width,
        .height = input->streams[video]->codecpar->height,
    };
    sha = av_sha_alloc();
    if (!sha) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    av_sha_init(sha, 256);
    av_sha_update(sha, data, size);
    av_sha_final(sha, result->content_fingerprint);
    result->has_content_fingerprint = 1;
    ret = 0;

cleanup:
    av_free(sha);
    av_dict_free(&options);
    avio_closep(&network);
    av_free(pts);
    av_packet_free(&packet);
    avformat_close_input(&input);
    if (memory_io) {
        av_freep(&memory_io->buffer);
        avio_context_free(&memory_io);
    }
    av_free(io_buffer);
    av_free(data);
    return ret;
}
