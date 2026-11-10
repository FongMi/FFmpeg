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

#include "config_components.h"
#include "libavutil/intreadwrite.h"
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

#if CONFIG_WAV_DEMUXER && CONFIG_FILE_PROTOCOL
typedef struct SingleFile {
    uint8_t header[44];
    int64_t size;
    int64_t pos;
    int64_t bytes_read;
    int read_error;
    int close_count;
} SingleFile;

static int read_single_file(void *opaque, uint8_t *buf, int size)
{
    SingleFile *file = opaque;
    int header_size = 0;

    if (file->read_error)
        return file->read_error;
    size = FFMIN(size, file->size - file->pos);
    if (!size)
        return AVERROR_EOF;
    if (file->pos < (int)sizeof(file->header)) {
        header_size = FFMIN(size, (int)sizeof(file->header) - file->pos);
        memcpy(buf, file->header + file->pos, header_size);
    }
    memset(buf + header_size, 0, size - header_size);
    file->pos += size;
    file->bytes_read += size;
    return size;
}

static int64_t seek_single_file(void *opaque, int64_t offset, int whence)
{
    SingleFile *file = opaque;

    if (whence == AVSEEK_SIZE)
        return file->size;
    if (whence != SEEK_SET || offset < 0 || offset > file->size)
        return AVERROR(EINVAL);
    return file->pos = offset;
}

static int open_single_file(AVFormatContext *s, AVIOContext **pb,
                            const char *url, int flags, AVDictionary **opts)
{
    SingleFile *file = s->opaque;
    const AVDictionaryEntry *entry = av_dict_get(*opts, "initial_request_size", NULL, 0);
    uint8_t *buf;

    if (!entry || strcmp(entry->value, "131072"))
        return AVERROR(EINVAL);
    buf = av_malloc(4096);
    if (!buf)
        return AVERROR(ENOMEM);
    file->pos = 0;
    *pb = avio_alloc_context(buf, 4096, 0, file, read_single_file, NULL,
                             seek_single_file);
    if (!*pb) {
        av_free(buf);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int close_single_file(AVFormatContext *s, AVIOContext *pb)
{
    SingleFile *file = s->opaque;

    file->close_count++;
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    return 0;
}

static int test_single_file(AVPacket *pkt)
{
    const int samples = 30 * 8000;
    SingleFile file = { .size = 44 + 2 * samples };
    char url[] = "file:dash-fixture.wav";
    char allowed_extensions[] = "wav";
    struct fragment fragment = { .url = url, .size = -1 };
    struct fragment *fragments[] = { &fragment };
    struct representation rep = { .fragments = fragments, .n_fragments = 1 };
    struct representation *audio[] = { &rep };
    DASHContext dash = {
        .audios = audio,
        .n_audios = FF_ARRAY_ELEMS(audio),
        .allowed_extensions = allowed_extensions,
        .max_url_size = 64,
    };
    AVFormatContext *format = avformat_alloc_context();
    AVStream *stream;
    int ret = 1;

    if (!format)
        return 1;

    memcpy(file.header, "RIFF", 4);
    AV_WL32(file.header + 4, file.size - 8);
    memcpy(file.header + 8, "WAVEfmt ", 8);
    AV_WL32(file.header + 16, 16);
    AV_WL16(file.header + 20, 1);
    AV_WL16(file.header + 22, 1);
    AV_WL32(file.header + 24, 8000);
    AV_WL32(file.header + 28, 16000);
    AV_WL16(file.header + 32, 2);
    AV_WL16(file.header + 34, 16);
    memcpy(file.header + 36, "data", 4);
    AV_WL32(file.header + 40, 2 * samples);

    format->priv_data = &dash;
    format->opaque = &file;
    format->io_open = open_single_file;
    format->io_close2 = close_single_file;
    dash.interrupt_callback = &format->interrupt_callback;
    rep.parent = format;
    stream = avformat_new_stream(format, NULL);
    if (!stream)
        goto end;
    stream->time_base = (AVRational) { 3, 1000 };
    rep.assoc_stream = format->streams;
    rep.nb_assoc_stream = 1;

    if (reopen_demux_for_component(format, &rep) < 0 ||
        rep.ctx->pb != rep.input || !rep.input->seekable || file.bytes_read > 4096) {
        fprintf(stderr, "DASH did not open the single file with a bounded seekable input\n");
        goto end;
    }
    if (dash_read_seek(format, 0, 5000, AVSEEK_FLAG_BACKWARD) < 0 ||
        av_read_frame(rep.ctx, pkt) < 0 || pkt->pts != 15 * 8000) {
        fprintf(stderr, "DASH did not seek to the requested sample in the single file\n");
        goto end;
    }
    av_packet_unref(pkt);
    stream->discard = AVDISCARD_ALL;
    if (dash_read_seek(format, 0, 7000, AVSEEK_FLAG_BACKWARD) < 0 ||
        rep.ctx || rep.input || file.close_count != 1) {
        fprintf(stderr, "DASH did not close the discarded single file\n");
        goto end;
    }
    stream->discard = AVDISCARD_DEFAULT;
    file.read_error = AVERROR(EIO);
    if (dash_read_seek(format, 0, 7000, AVSEEK_FLAG_BACKWARD) != file.read_error ||
        rep.ctx || rep.input || file.close_count != 2) {
        fprintf(stderr, "DASH did not propagate and clean up the failed open\n");
        goto end;
    }
    file.read_error = 0;
    if (dash_read_seek(format, 0, 7000, AVSEEK_FLAG_BACKWARD) < 0 ||
        !rep.ctx || av_read_frame(rep.ctx, pkt) < 0 || pkt->pts != 21 * 8000) {
        fprintf(stderr, "DASH did not seek in the newly selected single file\n");
        goto end;
    }
    ret = 0;

end:
    av_packet_unref(pkt);
    close_component(&rep);
    format->priv_data = NULL;
    avformat_free_context(format);
    return ret;
}
#endif

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
#if CONFIG_WAV_DEMUXER && CONFIG_FILE_PROTOCOL
    if (test_single_file(pkt))
        goto end;
#endif
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
