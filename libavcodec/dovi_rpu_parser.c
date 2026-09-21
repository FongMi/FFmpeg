/*
 * Dolby Vision RPU parser public API
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

#include <string.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "config.h"
#include "dovi_rpu_parser.h"
#if CONFIG_DOVI_RPUDEC
#include "dovi_rpu.h"
#endif

struct AVDOVIRpuParser {
#if CONFIG_DOVI_RPUDEC
    DOVIContext context;
    uint8_t *buffer;
    unsigned int buffer_size;
#else
    int unused;
#endif
};

#if CONFIG_DOVI_RPUDEC
static int unescape_rpu(AVDOVIRpuParser *parser, const uint8_t *data,
                        size_t size, size_t *unescaped_size)
{
    size_t input = 0;
    size_t output = 0;

    av_fast_padded_malloc(&parser->buffer, &parser->buffer_size, size);
    if (!parser->buffer)
        return AVERROR(ENOMEM);

    while (input + 2 < size) {
        if (input + 3 < size &&
            data[input] == 0 && data[input + 1] == 0 &&
            data[input + 2] == 3 && data[input + 3] <= 3) {
            parser->buffer[output++] = 0;
            parser->buffer[output++] = 0;
            input += 3;
        } else {
            parser->buffer[output++] = data[input++];
        }
    }
    while (input < size)
        parser->buffer[output++] = data[input++];

    memset(parser->buffer + output, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *unescaped_size = output;
    return 0;
}
#endif

AVDOVIRpuParser *av_dovi_rpu_parser_alloc(
        const AVDOVIDecoderConfigurationRecord *config)
{
#if CONFIG_DOVI_RPUDEC
    AVDOVIRpuParser *parser = av_mallocz(sizeof(*parser));
    if (!parser)
        return NULL;

    if (config)
        parser->context.cfg = *config;
    return parser;
#else
    (void) config;
    return NULL;
#endif
}

int av_dovi_rpu_parser_parse(AVDOVIRpuParser *parser,
                             const uint8_t *data, size_t size,
                             AVDOVIMetadata **metadata)
{
#if CONFIG_DOVI_RPUDEC
    int ret;
    int layer_id;
    int temporal_id;
    size_t payload_size;
    AVDOVIDecoderConfigurationRecord config;

    if (!metadata)
        return AVERROR(EINVAL);
    *metadata = NULL;
    if (!parser || !data || size < 3)
        return AVERROR(EINVAL);
    layer_id = ((data[0] & 0x01) << 5) | (data[1] >> 3);
    temporal_id = (data[1] & 0x07) - 1;
    if ((data[0] & 0x80) || ((data[0] & 0x7e) >> 1) != 62 ||
        layer_id || temporal_id)
        return AVERROR_INVALIDDATA;
    config = parser->context.cfg;

    ret = unescape_rpu(parser, data + 2, size - 2, &payload_size);
    if (ret < 0)
        return ret;
    ret = ff_dovi_rpu_parse(&parser->context, parser->buffer, payload_size, 0);
    if (ret < 0) {
        ff_dovi_ctx_flush(&parser->context);
        parser->context.cfg = config;
        return ret;
    }
    ret = ff_dovi_get_metadata(&parser->context, metadata);
    if (ret < 0) {
        ff_dovi_ctx_flush(&parser->context);
        parser->context.cfg = config;
    }
    return ret;
#else
    (void) parser;
    (void) data;
    (void) size;
    if (metadata)
        *metadata = NULL;
    return AVERROR(ENOSYS);
#endif
}

void av_dovi_rpu_parser_flush(AVDOVIRpuParser *parser)
{
#if CONFIG_DOVI_RPUDEC
    if (parser)
        ff_dovi_ctx_flush(&parser->context);
#else
    (void) parser;
#endif
}

void av_dovi_rpu_parser_free(AVDOVIRpuParser **parser)
{
    if (!parser || !*parser)
        return;
#if CONFIG_DOVI_RPUDEC
    ff_dovi_ctx_unref(&(*parser)->context);
    av_free((*parser)->buffer);
#endif
    av_freep(parser);
}
