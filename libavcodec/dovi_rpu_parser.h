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

#ifndef AVCODEC_DOVI_RPU_PARSER_H
#define AVCODEC_DOVI_RPU_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/dovi_meta.h"

/**
 * @file
 * Stateful Dolby Vision RPU parser.
 *
 * The parser retains stream state required by RPUs that reference previous
 * mappings. It is not thread-safe. One parser must be used per elementary
 * stream and calls must follow decoding order.
 */

typedef struct AVDOVIRpuParser AVDOVIRpuParser;

/**
 * Allocate a Dolby Vision RPU parser.
 *
 * @param config Optional stream configuration. The contents are copied.
 * @return A parser instance, or NULL when RPU parsing is unavailable or
 *         allocation fails.
 */
AVDOVIRpuParser *av_dovi_rpu_parser_alloc(
        const AVDOVIDecoderConfigurationRecord *config);

/**
 * Parse one complete HEVC Dolby Vision UNSPEC62 NAL unit.
 *
 * @param parser Parser instance.
 * @param data NAL unit including the two-byte HEVC NAL header and any
 *             emulation-prevention bytes, without a start code or length
 *             prefix.
 * @param size NAL unit size in bytes.
 * @param metadata Receives newly allocated metadata on success. Free it with
 *                 av_free(). The output is set to NULL if the current RPU does
 *                 not provide a complete mapping.
 * @return A non-negative metadata size, zero when metadata is incomplete, or
 *         a negative AVERROR code.
 */
int av_dovi_rpu_parser_parse(AVDOVIRpuParser *parser,
                             const uint8_t *data, size_t size,
                             AVDOVIMetadata **metadata);

/** Reset per-frame and inherited RPU state while preserving configuration. */
void av_dovi_rpu_parser_flush(AVDOVIRpuParser *parser);

/** Free a parser and set the caller's pointer to NULL. */
void av_dovi_rpu_parser_free(AVDOVIRpuParser **parser);

#endif /* AVCODEC_DOVI_RPU_PARSER_H */
