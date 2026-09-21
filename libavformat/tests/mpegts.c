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

#include "libavformat/mpegts.c"

static void write_syncsafe(uint8_t *dst, unsigned int value)
{
    dst[0] = value >> 21 & 0x7f;
    dst[1] = value >> 14 & 0x7f;
    dst[2] = value >> 7  & 0x7f;
    dst[3] = value       & 0x7f;
}

static int make_id3_packet(AVPacket *pkt, uint8_t *buffer, int arib)
{
    static const uint8_t owner[] = "aribb24.js";
    const unsigned int payload_size = sizeof(owner) + 3;
    const unsigned int tag_size = 10 + payload_size;

    memset(buffer, 0, 10 + tag_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(buffer, "ID3\x04\x00\x00", 6);
    write_syncsafe(buffer + 6, tag_size);
    memcpy(buffer + 10, "PRIV", 4);
    write_syncsafe(buffer + 14, payload_size);
    memcpy(buffer + 20, owner, sizeof(owner));
    buffer[20 + sizeof(owner)]     = arib ? 0x80 : 0x82;
    buffer[20 + sizeof(owner) + 1] = 0xff;
    buffer[20 + sizeof(owner) + 2] = 0x01;

    pkt->data = buffer;
    pkt->size = 10 + tag_size;
    return pkt->size;
}

static int test_aribb24_id3(void)
{
    uint8_t buffer[64 + AV_INPUT_BUFFER_PADDING_SIZE];
    AVFormatContext *format = avformat_alloc_context();
    AVStream *stream;
    PESContext pes = { 0 };
    AVPacket pkt = { 0 };
    int original_size;
    int ret = 1;

    if (!format)
        return 1;
    stream = avformat_new_stream(format, NULL);
    if (!stream)
        goto end;
    stream->codecpar->codec_id = AV_CODEC_ID_TIMED_ID3;
    pes.st = stream;
    pes.stream = format;

    original_size = make_id3_packet(&pkt, buffer, 1);
    if (!handle_aribb24_id3(&pes, &pkt) || !pes.id3_arib ||
        stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
        stream->codecpar->codec_id != AV_CODEC_ID_ARIB_CAPTION ||
        pkt.size != 3 || pkt.data[0] != 0x80 || pkt.data[1] != 0xff) {
        fprintf(stderr, "ARIB timed-ID3 packet was not unwrapped\n");
        goto end;
    }

    make_id3_packet(&pkt, buffer, 0);
    if (handle_aribb24_id3(&pes, &pkt) || pkt.size != 0 ||
        !(pes.flags & AV_PKT_FLAG_CORRUPT)) {
        fprintf(stderr, "mixed timed-ID3 packet was not dropped\n");
        goto end;
    }

    pes.id3_arib = 0;
    stream->codecpar->codec_id = AV_CODEC_ID_TIMED_ID3;
    pkt.flags = 0;
    make_id3_packet(&pkt, buffer, 0);
    if (handle_aribb24_id3(&pes, &pkt) || pkt.size != original_size) {
        fprintf(stderr, "ordinary timed-ID3 packet was modified\n");
        goto end;
    }

    ret = 0;
end:
    avformat_free_context(format);
    return ret;
}

int main(void)
{
    return test_aribb24_id3();
}
