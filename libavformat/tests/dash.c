/*
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

#include <stdio.h>

#include "libavformat/dashdec.c"

static int interrupt(void *opaque)
{
    return *(int *)opaque;
}

static int read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int *remaining = s->opaque;
    int ret;

    if (!*remaining)
        return AVERROR_EOF;
    ret = av_new_packet(pkt, 1);
    if (ret < 0)
        return ret;
    --*remaining;
    pkt->data[0] = 0;
    pkt->pts = pkt->dts = 0;
    return 0;
}

static const FFInputFormat input_format = {
    .p.name      = "dash-test",
    .p.flags     = AVFMT_NOFILE,
    .read_packet = read_packet,
};

int main(void)
{
    AVFormatContext *format = avformat_alloc_context();
    struct representation reps[2] = { 0 };
    struct representation *audio[] = { &reps[0], &reps[1] };
    DASHContext dash = { .audios = audio, .n_audios = 2 };
    AVPacket *pkt = av_packet_alloc();
    int remaining[] = { 0, 1 };
    int cancelled = 1;
    int ret = 1;

    if (!format || !pkt)
        goto end;
    format->priv_data = &dash;
    format->interrupt_callback = (AVIOInterruptCB) { interrupt, &cancelled };
    dash.interrupt_callback = &format->interrupt_callback;

    for (int i = 0; i < 2; i++) {
        AVStream *stream = avformat_new_stream(format, NULL);
        if (!stream)
            goto end;
        reps[i].ctx = avformat_alloc_context();
        if (!reps[i].ctx)
            goto end;
        reps[i].ctx->iformat = &input_format.p;
        reps[i].ctx->opaque = &remaining[i];
        stream = avformat_new_stream(reps[i].ctx, NULL);
        if (!stream)
            goto end;
        stream->time_base = (AVRational) { 1, 90000 };
        stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        stream->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
        reps[i].parent = format;
        reps[i].stream_index = i;
        reps[i].nb_assoc_stream = 1;
    }
    for (int i = 0; i < 2; i++)
        reps[i].assoc_stream = &format->streams[i];

    if (dash_read_packet(format, pkt) != AVERROR_EXIT || pkt->size) {
        fprintf(stderr, "DASH cancellation did not return AVERROR_EXIT\n");
        goto end;
    }
    cancelled = 0;
    if (dash_read_packet(format, pkt) != FFERROR_REDO || reps[0].ctx ||
        format->streams[0]->discard != AVDISCARD_ALL) {
        fprintf(stderr, "DASH did not retire the first exhausted representation\n");
        goto end;
    }
    if (dash_read_packet(format, pkt) || pkt->stream_index != 1 || pkt->size != 1) {
        fprintf(stderr, "DASH lost the remaining representation's packet\n");
        goto end;
    }
    av_packet_unref(pkt);
    if (dash_read_packet(format, pkt) != FFERROR_REDO ||
        dash_read_packet(format, pkt) != AVERROR_EOF) {
        fprintf(stderr, "DASH did not reach EOF after all representations\n");
        goto end;
    }
    ret = 0;

end:
    for (int i = 0; i < 2; i++)
        avformat_close_input(&reps[i].ctx);
    if (format)
        format->priv_data = NULL;
    avformat_free_context(format);
    av_packet_free(&pkt);
    return ret;
}
