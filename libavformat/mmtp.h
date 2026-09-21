/*
 * MPEG Media Transport Protocol (MMTP) parser, as defined in ISO/IEC 23008-1.
 * Copyright (c) 2025 SuperFashi G.K.
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
#ifndef AVFORMAT_MMTP_H
#define AVFORMAT_MMTP_H

#include "avformat.h"

typedef struct MMTPContext MMTPContext;

/**
 * Open an MMT protocol parser context.
 * @param program The AVProgram this context is associated with.
 * @return A new MMTPContext, or NULL on allocation error.
 */
MMTPContext *ff_mmtp_parse_open(AVProgram *program);

/**
 * Parse an MMT protocol packet.
 *
 * @param ctx The MMT protocol parser context.
 * @param s The AVFormatContext.
 * @param pkt The AVPacket to fill.
 * @param buf The packet data.
 * @param size The size of the packet data.
 * @return >= 0 if a new AVPacket is emitted,
 *         FFERROR_REDO if the next packet is needed,
 *         or another negative value on error.
 */
int ff_mmtp_parse_packet(MMTPContext *ctx, AVFormatContext *s, AVPacket *pkt,
                         const uint8_t *buf, uint16_t size);

/**
 * Return the next packet buffered while parsing an aggregated MMTP payload.
 *
 * @param ctx The MMT protocol parser context.
 * @param pkt The AVPacket to fill.
 * @return 0 if a packet is returned, or AVERROR(EAGAIN) if none is buffered.
 */
int ff_mmtp_get_packet(MMTPContext *ctx, AVPacket *pkt);

/**
 * Set the NTP long-format timestamp used to expand MMTP short timestamps.
 *
 * @param ctx The MMT protocol parser context.
 * @param ntp_anchor The NTP long-format timestamp.
 */
void ff_mmtp_set_ntp_anchor(MMTPContext *ctx, uint64_t ntp_anchor);

/**
 * Return the NTP long-format timestamp used to expand MMTP short timestamps.
 */
uint64_t ff_mmtp_get_ntp_anchor(const MMTPContext *ctx);

/**
 * Reset buffered MMTP fragments and codec parser state after an input
 * discontinuity or resynchronization.
 *
 * @param ctx The MMT protocol parser context.
 */
void ff_mmtp_reset_state(MMTPContext *ctx);

/**
 * Close an MMT protocol parser context, freeing all associated resources.
 *
 * @param ctx The MMT protocol parser context.
 */
void ff_mmtp_parse_close(MMTPContext *ctx);

#endif /* AVFORMAT_MMTP_H */
