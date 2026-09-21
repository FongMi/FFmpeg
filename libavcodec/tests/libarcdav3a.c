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

/* Exercise the wrapper's packet ownership with deterministic decoder results.
 * The external API parses a complete header before checking payload length. */
#define parse_header test_parse_header
#define avs3_decode test_avs3_decode
#define ff_libarcdav3a_decoder test_libarcdav3a_decoder
#include "libavcodec/libarcdav3a.c"

#define HEADER_SIZE  9
#define PAYLOAD_SIZE 64
#define FRAME_SIZE   (HEADER_SIZE + PAYLOAD_SIZE)

static const uint8_t header[HEADER_SIZE] = {
    0xff, 0xfe, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
};

int test_parse_header(AVS3DecoderHandle decoder, unsigned char *data, int size,
                      int first_frame, int *consumed, unsigned short *crc)
{
    *consumed = 0;
    if (size < HEADER_SIZE)
        return AVS3_DATA_NOT_ENOUGH;
    if (memcmp(data, header, HEADER_SIZE)) {
        *consumed = size;
        return AVS3_FALSE;
    }
    decoder->numChansOutput = 2;
    decoder->frameLength = PAYLOAD_SIZE / 4;
    decoder->channelNumConfig = CHANNEL_CONFIG_STEREO;
    decoder->outputFs = 48000;
    decoder->totalBitrate = 64000;
    *consumed = HEADER_SIZE;
    return AVS3_TRUE;
}

int test_avs3_decode(AVS3DecoderHandle decoder, unsigned char *data, int size,
                     unsigned char *output, int *output_size, int *consumed)
{
    *output_size = *consumed = 0;
    if (size < PAYLOAD_SIZE)
        return AVS3_DATA_NOT_ENOUGH;
    memcpy(output, data, PAYLOAD_SIZE);
    *output_size = *consumed = PAYLOAD_SIZE;
    return AVS3_TRUE;
}

static int decode_chunk(AVCodecContext *avctx, const uint8_t *data, int size,
                        const uint8_t *expected, int expected_size,
                        const uint8_t *pending, int pending_size)
{
    LibARCDAV3AContext *s = avctx->priv_data;
    int consumed = 0;
    int ret = libarcdav3a_decode_buffer(avctx, data, size, &consumed, &s->output);

    if (ret < 0 || consumed != size || s->output.size != expected_size ||
        (expected_size && memcmp(s->output.data, expected, expected_size)) ||
        s->buffered_size != pending_size ||
        (pending_size && memcmp(s->buffer, pending, pending_size)) ||
        s->error_count) {
        fprintf(stderr, "AV3A chunk %d: ret=%d, consumed=%d, output=%d, pending=%d, errors=%d\n",
                size, ret, consumed, s->output.size, s->buffered_size, s->error_count);
        return 1;
    }
    return 0;
}

static int test_packets(const uint8_t *frame, int chunk_size)
{
    LibARCDAV3AContext s = { 0 };
    AVCodecContext avctx = { .priv_data = &s };
    int ret = 1;

    s.decoder = av_mallocz(sizeof(*s.decoder));
    s.output.data = av_malloc(AV3A_MAX_OUTPUT_SIZE);
    if (!s.decoder || !s.output.data)
        goto end;
    libarcdav3a_reset_state(&s);

    for (int pos = 0; pos < FRAME_SIZE;) {
        int size = FFMIN(chunk_size, FRAME_SIZE - pos);
        int complete = pos + size == FRAME_SIZE;

        if (decode_chunk(&avctx, frame + pos, size, frame + HEADER_SIZE,
                         complete ? PAYLOAD_SIZE : 0, frame,
                         complete ? 0 : pos + size) ||
            s.first_frame != !complete)
            goto end;
        pos += size;
    }
    ret = 0;

end:
    av_freep(&s.decoder);
    av_freep(&s.output.data);
    av_freep(&s.buffer);
    return ret;
}

static int test_two_packets(const uint8_t *frame, int split)
{
    LibARCDAV3AContext s = { 0 };
    AVCodecContext avctx = { .priv_data = &s };
    uint8_t data[2 * FRAME_SIZE];
    int ret = 1;

    memcpy(data, frame, FRAME_SIZE);
    memcpy(data + FRAME_SIZE, frame, FRAME_SIZE);
    s.decoder = av_mallocz(sizeof(*s.decoder));
    s.output.data = av_malloc(AV3A_MAX_OUTPUT_SIZE);
    if (!s.decoder || !s.output.data)
        goto end;
    libarcdav3a_reset_state(&s);

    if (decode_chunk(&avctx, data, FRAME_SIZE + split, frame + HEADER_SIZE,
                     PAYLOAD_SIZE, frame, split) ||
        decode_chunk(&avctx, data + FRAME_SIZE + split, FRAME_SIZE - split,
                     frame + HEADER_SIZE, PAYLOAD_SIZE, NULL, 0))
        goto end;
    ret = 0;

end:
    av_freep(&s.decoder);
    av_freep(&s.output.data);
    av_freep(&s.buffer);
    return ret;
}

static int test_allocation_failure(const uint8_t *frame)
{
    LibARCDAV3AContext s = { 0 };
    AVCodecContext avctx = { .priv_data = &s };
    uint8_t *buffer;
    int capacity, consumed = 0, ret = 1;

    s.decoder = av_mallocz(sizeof(*s.decoder));
    s.output.data = av_malloc(AV3A_MAX_OUTPUT_SIZE);
    if (!s.decoder || !s.output.data)
        goto end;
    libarcdav3a_reset_state(&s);
    if (decode_chunk(&avctx, frame, HEADER_SIZE, NULL, 0, frame, HEADER_SIZE))
        goto end;
    buffer = s.buffer;
    capacity = s.buffer_size;

    av_max_alloc(1);
    ret = libarcdav3a_decode_buffer(&avctx, frame + HEADER_SIZE, PAYLOAD_SIZE,
                                    &consumed, &s.output);
    av_max_alloc(INT_MAX);
    if (ret != AVERROR(ENOMEM) || consumed || s.buffer != buffer ||
        s.buffer_size != capacity || s.buffered_size != HEADER_SIZE ||
        memcmp(s.buffer, frame, HEADER_SIZE)) {
        fprintf(stderr, "AV3A allocation failure discarded buffered input\n");
        ret = 1;
        goto end;
    }
    ret = decode_chunk(&avctx, frame + HEADER_SIZE, PAYLOAD_SIZE,
                       frame + HEADER_SIZE, PAYLOAD_SIZE, NULL, 0);

end:
    av_freep(&s.decoder);
    av_freep(&s.output.data);
    av_freep(&s.buffer);
    return ret;
}

int main(void)
{
    uint8_t frame[FRAME_SIZE];

    memcpy(frame, header, HEADER_SIZE);
    for (int i = HEADER_SIZE; i < FRAME_SIZE; i++)
        frame[i] = i;

    if (test_packets(frame, FRAME_SIZE) || test_packets(frame, 1))
        return 1;
    for (int split = 1; split < FRAME_SIZE; split++) {
        if (test_packets(frame, split) || test_two_packets(frame, split))
            return 1;
    }
    return test_allocation_failure(frame);
}
