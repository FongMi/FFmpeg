/*
 * MMT protocol over TLV packets (MMT/TLV) demuxer, as defined in
 * ARIB STD-B60 and ARIB STD-B32.
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

#include <stdbool.h>

#include "config_components.h"

#include "libavutil/mem.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "mmtp.h"

#define HEADER_BYTE 0x7f
#define TLV_MAX_PACKET_SIZE (4 + UINT16_MAX)
#define MAX_MMT_PROGRAMS 8
#define MAX_COMPRESSED_IP_CONTEXTS (1 << 12)
#define UNCOMPRESSED_CONTEXT_ID 0x1000

#define IP_PROTOCOL_UDP 17
#define IPV4_MIN_HEADER_SIZE 20
#define IPV6_HEADER_SIZE 40
#define UDP_HEADER_SIZE 8
#define UDP_PORT_NTP 123
#define NTP_PACKET_SIZE 48

#define PARTIAL_IPV4_HEADER_SIZE 16
#define IPV4_IDENTIFICATION_SIZE 2
#define PARTIAL_IPV6_HEADER_SIZE 38
#define PARTIAL_UDP_HEADER_SIZE 4

enum {
    UNDEFINED_PACKET            = 0x00,
    IPV4_PACKET                 = 0x01,
    IPV6_PACKET                 = 0x02,
    HEADER_COMPRESSED_IP_PACKET = 0x03,
    TRANSMISSION_CONTROL_PACKET = 0xFE,
    NULL_PACKET                 = 0xFF,
};

enum CompressedIPProtocol {
    COMPRESSED_IP_PROTOCOL_UNKNOWN,
    COMPRESSED_IP_PROTOCOL_MMTP,
    COMPRESSED_IP_PROTOCOL_NTP,
    COMPRESSED_IP_PROTOCOL_UNSUPPORTED,
};

struct IPUDPFlow {
    uint8_t  address_size;
    uint8_t  source_address[16];
    uint8_t  destination_address[16];
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t context_id;
};

static int is_tlv_packet_type(uint8_t packet_type)
{
    switch (packet_type) {
    case UNDEFINED_PACKET:
    case IPV4_PACKET:
    case IPV6_PACKET:
    case HEADER_COMPRESSED_IP_PACKET:
    case TRANSMISSION_CONTROL_PACKET:
    case NULL_PACKET:
        return 1;
    default:
        return 0;
    }
}

static int skip_tlv_packet(const AVPacket *pkt)
{
    return pkt == NULL ? 0 : FFERROR_REDO;
}

enum {
    CONTEXT_IDENTIFICATION_PARTIAL_IPV4_AND_PARTIAL_UDP_HEADER = 0x20,
    CONTEXT_IDENTIFICATION_IPV4_HEADER                         = 0x21,
    CONTEXT_IDENTIFICATION_PARTIAL_IPV6_AND_PARTIAL_UDP_HEADER = 0x60,
    CONTEXT_IDENTIFICATION_NO_COMPRESSED_HEADER                = 0x61,
};

static int compressed_ip_header_size(uint8_t context_type)
{
    switch (context_type) {
    case CONTEXT_IDENTIFICATION_PARTIAL_IPV4_AND_PARTIAL_UDP_HEADER:
        return PARTIAL_IPV4_HEADER_SIZE + PARTIAL_UDP_HEADER_SIZE;
    case CONTEXT_IDENTIFICATION_IPV4_HEADER:
        return IPV4_IDENTIFICATION_SIZE;
    case CONTEXT_IDENTIFICATION_PARTIAL_IPV6_AND_PARTIAL_UDP_HEADER:
        return PARTIAL_IPV6_HEADER_SIZE + PARTIAL_UDP_HEADER_SIZE;
    case CONTEXT_IDENTIFICATION_NO_COMPRESSED_HEADER:
        return 0;
    default:
        return AVERROR_INVALIDDATA;
    }
}

static int is_mmtp_packet(const uint8_t *buf, uint16_t size)
{
    if (size < 12 || buf[0] >> 6)
        return 0;
    return 1;
}

static int get_ntp_transmit_timestamp(const uint8_t *buf, uint16_t size,
                                      int strict, uint64_t *timestamp)
{
    int version;
    int mode;

    if (size < NTP_PACKET_SIZE)
        return 0;
    version = buf[0] >> 3 & 0x07;
    mode    = buf[0] & 0x07;
    if (version != 4 ||
        (mode != 4 && mode != 5) ||
        !AV_RB64(buf + 40))
        return 0;
    if (strict &&
        (mode != 5 || buf[1] < 1 || buf[1] > 15 ||
         AV_RB32(buf + 12) || AV_RB64(buf + 24) ||
         AV_RB64(buf + 32)))
        return 0;

    *timestamp = AV_RB64(buf + 40);
    return 1;
}

static int get_ip_udp_payload(const uint8_t *buf, uint16_t size,
                              int packet_type, const uint8_t **payload,
                              uint16_t *payload_size, struct IPUDPFlow *flow)
{
    const uint8_t *udp;
    const uint8_t *source_address;
    const uint8_t *destination_address;
    uint8_t address_size;
    unsigned int ip_header_size;
    unsigned int ip_packet_size;
    unsigned int udp_available;
    unsigned int udp_size;

    if (packet_type == IPV4_PACKET) {
        if (size < IPV4_MIN_HEADER_SIZE || buf[0] >> 4 != 4)
            return AVERROR_INVALIDDATA;
        ip_header_size = (buf[0] & 0x0F) * 4;
        ip_packet_size = AV_RB16(buf + 2);
        if (ip_header_size < IPV4_MIN_HEADER_SIZE ||
            ip_packet_size < ip_header_size ||
            ip_packet_size > size ||
            (AV_RB16(buf + 6) & 0x3FFF) ||
            buf[9] != IP_PROTOCOL_UDP)
            return AVERROR_INVALIDDATA;
        address_size = 4;
        source_address = buf + 12;
        destination_address = buf + 16;
    } else if (packet_type == IPV6_PACKET) {
        if (size < IPV6_HEADER_SIZE || buf[0] >> 4 != 6 ||
            buf[6] != IP_PROTOCOL_UDP)
            return AVERROR_INVALIDDATA;
        ip_header_size = IPV6_HEADER_SIZE;
        ip_packet_size = IPV6_HEADER_SIZE + AV_RB16(buf + 4);
        if (ip_packet_size > size)
            return AVERROR_INVALIDDATA;
        address_size = 16;
        source_address = buf + 8;
        destination_address = buf + 24;
    } else {
        return AVERROR_BUG;
    }

    udp_available = ip_packet_size - ip_header_size;
    if (udp_available < UDP_HEADER_SIZE)
        return AVERROR_INVALIDDATA;
    udp = buf + ip_header_size;
    udp_size = AV_RB16(udp + 4);
    if (udp_size < UDP_HEADER_SIZE || udp_size > udp_available)
        return AVERROR_INVALIDDATA;

    if (flow) {
        flow->address_size = address_size;
        memcpy(flow->source_address, source_address, address_size);
        memcpy(flow->destination_address, destination_address,
               address_size);
        flow->source_port = AV_RB16(udp);
        flow->destination_port = AV_RB16(udp + 2);
    }
    *payload = udp + UDP_HEADER_SIZE;
    *payload_size = udp_size - UDP_HEADER_SIZE;
    return 0;
}

static int mmttlv_probe(const AVProbeData *p)
{
    int      i, j, packet_end, packet_start;
    uint8_t  packet_type;
    uint16_t data_length;

    int processed  = 0;
    int recognized = 0;
    int last_end = -1;
    int chain = 0;
    int longest_chain = 0;

    for (i = 0; i + 4 <= p->buf_size && processed < 100;) {
        if (p->buf[i] != HEADER_BYTE) {
            ++i;
            continue;
        }
        packet_start = i;
        packet_type = p->buf[i + 1];
        data_length = AV_RB16(p->buf + i + 2);
        if (!is_tlv_packet_type(packet_type) ||
            data_length > p->buf_size - i - 4) {
            ++i;
            continue;
        }
        packet_end = i + 4 + data_length;
        i = packet_end;
        ++processed;

        if (packet_type == HEADER_COMPRESSED_IP_PACKET) {
            int compressed_header_size;

            if (data_length < 3)
                continue;
            compressed_header_size =
                compressed_ip_header_size(p->buf[packet_start + 6]);
            if (compressed_header_size < 0 ||
                data_length < 3 + compressed_header_size)
                continue;
        } else if (packet_type == IPV4_PACKET ||
                   packet_type == IPV6_PACKET) {
            const uint8_t *payload;
            uint16_t payload_size;

            if (get_ip_udp_payload(p->buf + packet_start + 4, data_length,
                                   packet_type, &payload, &payload_size,
                                   NULL) < 0)
                continue;
        } else if (packet_type == NULL_PACKET) {
            if (data_length < 1)
                continue;
            // null packets should contain all 0xFFs
            for (j = packet_start + 4; j < packet_end; ++j)
                if (p->buf[j] != 0xFF)
                    break;
            if (j != packet_end)
                continue;
        } else {
            continue;
        }

        ++recognized;
        chain = packet_start == last_end ? chain + 1 : 1;
        longest_chain = FFMAX(longest_chain, chain);
        last_end = packet_end;
    }

    /*
     * Two packets with valid TLV boundaries and plausible IP payloads are
     * stronger evidence than a server-provided MIME type.
     * This matters for broadcasters that label MMT/TLV as video/MP2T.
     */
    if (longest_chain >= 2)
        return AVPROBE_SCORE_MAX;
    return recognized * AVPROBE_SCORE_MAX / FFMAX(processed, 10);
}

struct MMTTLVContext {
    struct Program {
        uint32_t       cid;
        MMTPContext    *mmtp;
        struct Program *next;
    } *programs;
    AVStream *tlv_si_stream;

    int64_t last_pos;
    int64_t resync_size;
    int64_t timestamp_origin_us;
    unsigned int nb_programs;
    unsigned int nb_raw_flows;
    uint64_t ntp_anchor;

    uint8_t compressed_ip_protocols[MAX_COMPRESSED_IP_CONTEXTS];
    struct IPUDPFlow raw_flows[MAX_MMT_PROGRAMS];
    size_t  cap;
    uint8_t *buf;
};

static int mmttlv_read_mmtp_packet(
    struct MMTTLVContext *ctx, AVFormatContext *s, AVPacket *pkt,
    uint32_t context_id, const uint8_t *buf, uint16_t size);

static void mmttlv_set_ntp_anchor(struct MMTTLVContext *ctx,
                                  uint64_t ntp_anchor)
{
    struct Program *program;

    ctx->ntp_anchor = ntp_anchor;
    for (program = ctx->programs; program; program = program->next)
        ff_mmtp_set_ntp_anchor(program->mmtp, ntp_anchor);
}

static enum CompressedIPProtocol
compressed_udp_protocol(uint8_t ip_protocol, const uint8_t *udp)
{
    if (ip_protocol != IP_PROTOCOL_UDP)
        return COMPRESSED_IP_PROTOCOL_UNSUPPORTED;
    if (AV_RB16(udp) == UDP_PORT_NTP ||
        AV_RB16(udp + 2) == UDP_PORT_NTP)
        return COMPRESSED_IP_PROTOCOL_NTP;
    return COMPRESSED_IP_PROTOCOL_MMTP;
}

static int mmttlv_read_ntp_packet(struct MMTTLVContext *ctx,
                                  const AVPacket *pkt,
                                  const uint8_t *buf, uint16_t size,
                                  int strict)
{
    uint64_t ntp_anchor;

    if (!get_ntp_transmit_timestamp(buf, size, strict, &ntp_anchor))
        return strict ? AVERROR_INVALIDDATA : skip_tlv_packet(pkt);
    mmttlv_set_ntp_anchor(ctx, ntp_anchor);
    return skip_tlv_packet(pkt);
}

static int mmttlv_read_compressed_ip_packet(
    struct MMTTLVContext *ctx, AVFormatContext *s, AVPacket *pkt,
    const uint8_t *buf, uint16_t size)
{
    int      compressed_header_size;
    enum CompressedIPProtocol protocol;
    uint32_t context_id;
    uint8_t  context_type;

    if (size < 3)
        return AVERROR_INVALIDDATA;
    context_id = AV_RB16(buf) >> 4;
    context_type = buf[2];
    buf += 3;
    size -= 3;

    compressed_header_size = compressed_ip_header_size(context_type);
    if (compressed_header_size < 0 || size < compressed_header_size)
        return AVERROR_INVALIDDATA;

    switch (context_type) {
    case CONTEXT_IDENTIFICATION_PARTIAL_IPV4_AND_PARTIAL_UDP_HEADER:
        protocol = compressed_udp_protocol(
            buf[7], buf + PARTIAL_IPV4_HEADER_SIZE);
        ctx->compressed_ip_protocols[context_id] = protocol;
        break;
    case CONTEXT_IDENTIFICATION_PARTIAL_IPV6_AND_PARTIAL_UDP_HEADER:
        protocol = compressed_udp_protocol(
            buf[4], buf + PARTIAL_IPV6_HEADER_SIZE);
        ctx->compressed_ip_protocols[context_id] = protocol;
        break;
    case CONTEXT_IDENTIFICATION_IPV4_HEADER:
    case CONTEXT_IDENTIFICATION_NO_COMPRESSED_HEADER:
        break;
    }
    size -= compressed_header_size;
    buf  += compressed_header_size;

    protocol = ctx->compressed_ip_protocols[context_id];
    if (protocol == COMPRESSED_IP_PROTOCOL_UNKNOWN) {
        uint64_t ntp_anchor;

        if (get_ntp_transmit_timestamp(buf, size, 1, &ntp_anchor)) {
            ctx->compressed_ip_protocols[context_id] =
                COMPRESSED_IP_PROTOCOL_NTP;
            mmttlv_set_ntp_anchor(ctx, ntp_anchor);
            return skip_tlv_packet(pkt);
        }
        if (!is_mmtp_packet(buf, size))
            return skip_tlv_packet(pkt);
        ctx->compressed_ip_protocols[context_id] =
            COMPRESSED_IP_PROTOCOL_MMTP;
        protocol = COMPRESSED_IP_PROTOCOL_MMTP;
    }
    if (protocol == COMPRESSED_IP_PROTOCOL_NTP)
        return mmttlv_read_ntp_packet(ctx, pkt, buf, size, 0);
    if (protocol != COMPRESSED_IP_PROTOCOL_MMTP)
        return skip_tlv_packet(pkt);
    return mmttlv_read_mmtp_packet(ctx, s, pkt, context_id, buf, size);
}

static int mmttlv_read_mmtp_packet(
    struct MMTTLVContext *ctx, AVFormatContext *s, AVPacket *pkt,
    uint32_t context_id, const uint8_t *buf, uint16_t size)
{
    struct Program *program;

    if (!is_mmtp_packet(buf, size))
        return skip_tlv_packet(pkt);

    for (program = ctx->programs; program; program = program->next)
        if (program->cid == context_id)
            break;

    if (program == NULL) {
        AVProgram *p;

        if (ctx->nb_programs >= MAX_MMT_PROGRAMS)
            return AVERROR_INVALIDDATA;
        p = av_new_program(s, context_id);
        if (p == NULL)
            return AVERROR(ENOMEM);

        program = av_malloc(sizeof(struct Program));
        if (program == NULL)
            return AVERROR(ENOMEM);

        // av_malloc does not set errno; and an unchecked NULL mmtp would be dereferenced by
        // ff_mmtp_parse_packet below.
        program->mmtp = ff_mmtp_parse_open(p);
        if (program->mmtp == NULL) {
            av_free(program);
            return AVERROR(ENOMEM);
        }
        program->next = ctx->programs;
        ctx->programs = program;
        program->cid  = context_id;
        ff_mmtp_set_ntp_anchor(program->mmtp, ctx->ntp_anchor);
        ctx->nb_programs++;
    }

    {
        int err = ff_mmtp_parse_packet(program->mmtp, s, pkt, buf, size);
        uint64_t ntp_anchor = ff_mmtp_get_ntp_anchor(program->mmtp);

        if (err == AVERROR_INVALIDDATA || err == AVERROR_PATCHWELCOME) {
            av_log(s, AV_LOG_WARNING,
                   "Invalid or unsupported MMTP packet for context 0x%03x; "
                   "resetting fragment state\n", context_id);
            ff_mmtp_reset_state(program->mmtp);
            return skip_tlv_packet(pkt);
        }
        if (ntp_anchor && ntp_anchor != ctx->ntp_anchor)
            mmttlv_set_ntp_anchor(ctx, ntp_anchor);
        return err;
    }
}

static int get_raw_flow_context_id(struct MMTTLVContext *ctx,
                                   const struct IPUDPFlow *flow,
                                   uint32_t *context_id)
{
    unsigned int i;

    for (i = 0; i < ctx->nb_raw_flows; i++) {
        const struct IPUDPFlow *candidate = &ctx->raw_flows[i];

        if (candidate->address_size == flow->address_size &&
            candidate->source_port == flow->source_port &&
            candidate->destination_port == flow->destination_port &&
            !memcmp(candidate->source_address, flow->source_address,
                    flow->address_size) &&
            !memcmp(candidate->destination_address, flow->destination_address,
                    flow->address_size)) {
            *context_id = candidate->context_id;
            return 0;
        }
    }
    if (ctx->nb_raw_flows >= FF_ARRAY_ELEMS(ctx->raw_flows))
        return AVERROR(ENOSPC);

    ctx->raw_flows[ctx->nb_raw_flows] = *flow;
    ctx->raw_flows[ctx->nb_raw_flows].context_id =
        UNCOMPRESSED_CONTEXT_ID + ctx->nb_raw_flows;
    *context_id = ctx->raw_flows[ctx->nb_raw_flows].context_id;
    ctx->nb_raw_flows++;
    return 0;
}

static int mmttlv_read_ip_packet(
    struct MMTTLVContext *ctx, AVFormatContext *s, AVPacket *pkt,
    const uint8_t *buf, uint16_t size, int packet_type)
{
    const uint8_t *payload;
    struct IPUDPFlow flow;
    uint32_t context_id;
    uint16_t payload_size;

    if (get_ip_udp_payload(buf, size, packet_type,
                           &payload, &payload_size, &flow) < 0)
        return skip_tlv_packet(pkt);
    if (flow.source_port == UDP_PORT_NTP ||
        flow.destination_port == UDP_PORT_NTP)
        return mmttlv_read_ntp_packet(ctx, pkt, payload, payload_size, 0);
    if (!is_mmtp_packet(payload, payload_size))
        return skip_tlv_packet(pkt);
    if (get_raw_flow_context_id(ctx, &flow, &context_id) < 0)
        return skip_tlv_packet(pkt);
    return mmttlv_read_mmtp_packet(ctx, s, pkt, context_id,
                                   payload, payload_size);
}

static int mmttlv_read_tlv_si_packet(struct MMTTLVContext *ctx,
                                     AVFormatContext *s, AVPacket *pkt,
                                     const uint8_t *buf, uint16_t size)
{
    int err;

    if (!pkt)
        return 0;
    if (!ctx->tlv_si_stream) {
        AVStream *stream = avformat_new_stream(s, NULL);

        if (!stream)
            return AVERROR(ENOMEM);
        stream->id                   = 0x7f000000;
        stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
        stream->codecpar->codec_tag  = MKTAG('t', 'l', 'v', 's');
        stream->disposition         |= AV_DISPOSITION_METADATA;
        avpriv_set_pts_info(stream, 64, 1, 1000);
        if ((err = av_dict_set(&stream->metadata, "title", "TLV-SI", 0)) < 0)
            return err;
        ctx->tlv_si_stream = stream;
    }
    if ((err = av_new_packet(pkt, size)) < 0)
        return err;
    memcpy(pkt->data, buf, size);
    pkt->stream_index = ctx->tlv_si_stream->index;
    pkt->flags       |= AV_PKT_FLAG_KEY;
    return 0;
}

static void normalize_packet_timestamps(struct MMTTLVContext *ctx,
                                        AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st;
    bool is_ttml;
    int64_t reference;
    int64_t origin;

    if (pkt == NULL || pkt->stream_index < 0 ||
        pkt->stream_index >= (int) s->nb_streams)
        return;

    st = s->streams[pkt->stream_index];
    is_ttml = st->codecpar->codec_id == AV_CODEC_ID_TTML;
    if (is_ttml && pkt->dts != AV_NOPTS_VALUE)
        reference = pkt->dts;
    else
        reference = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (reference == AV_NOPTS_VALUE)
        return;

    if (ctx->timestamp_origin_us == AV_NOPTS_VALUE) {
        ctx->timestamp_origin_us = av_rescale_q(reference, st->time_base,
                                               AV_TIME_BASE_Q);
        s->start_time_realtime = ctx->timestamp_origin_us;
    }

    origin = av_rescale_q(ctx->timestamp_origin_us, AV_TIME_BASE_Q,
                          st->time_base);

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= origin;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= origin;
}

static int mmttlv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    uint8_t              header[4], next_header[4];
    uint16_t             size;
    int                  err;
    struct MMTTLVContext *ctx = s->priv_data;
    int64_t              pos  = avio_tell(s->pb);

    if (pos < 0)
        return (int) pos;
    if (pos != ctx->last_pos) {
        ctx->last_pos = pos;

        while (pos - ctx->last_pos < ctx->resync_size) {
            if ((err = ffio_ensure_seekback(s->pb, 4)) < 0)
                return err;

            if ((err = ffio_read_size(s->pb, header, 4)) < 0)
                return avio_feof(s->pb) ? AVERROR_EOF : err;

            if (header[0] != HEADER_BYTE ||
                !is_tlv_packet_type(header[1])) {
                if ((pos = avio_seek(s->pb, -3, SEEK_CUR)) < 0)
                    return (int) pos;
                continue;
            }

            size = AV_RB16(header + 2);

            if ((pos = avio_seek(s->pb, -4, SEEK_CUR)) < 0)
                return (int) pos;

            if ((err = ffio_ensure_seekback(s->pb, 4 + size + 4)) < 0)
                return err;

            if ((pos = avio_skip(s->pb, 4 + size)) < 0)
                return (int) pos;

            if ((err = ffio_read_size(s->pb, next_header, 4)) < 0)
                return avio_feof(s->pb) ? AVERROR_EOF : err;

            if (next_header[0] == HEADER_BYTE &&
                is_tlv_packet_type(next_header[1])) {
                // found HEADER, [size], HEADER, should be good
                if ((pos = avio_seek(
                    s->pb, -(int64_t) (size) - 8, SEEK_CUR)) < 0)
                    return (int) pos;
                goto success;
            }

            if ((pos = avio_seek(
                s->pb, -(int64_t) (size) - 7, SEEK_CUR)) < 0)
                return (int) pos;
        }
        return AVERROR_INVALIDDATA;

        success:
        ctx->last_pos = pos;
        ctx->ntp_anchor = 0;

        for (struct Program *program = ctx->programs;
             program; program = program->next) {
            ff_mmtp_reset_state(program->mmtp);
        }
    }

    if (pkt) {
        struct Program *program;

        for (program = ctx->programs; program; program = program->next) {
            err = ff_mmtp_get_packet(program->mmtp, pkt);
            if (err >= 0) {
                normalize_packet_timestamps(ctx, s, pkt);
                return 0;
            }
            if (err != AVERROR(EAGAIN))
                return err;
        }
    }

    if (pkt != NULL)
        pkt->pos = ctx->last_pos;
    if ((err = ffio_read_size(s->pb, header, 4)) < 0)
        return avio_feof(s->pb) ? AVERROR_EOF : err;
    ctx->last_pos += 4;

    if (header[0] != HEADER_BYTE || !is_tlv_packet_type(header[1])) {
        if ((pos = avio_seek(s->pb, -3, SEEK_CUR)) < 0)
            return (int) pos;
        ctx->last_pos = pos - 1;
        return skip_tlv_packet(pkt);
    }

    size = AV_RB16(header + 2);
    if (header[1] == NULL_PACKET) {
        if ((ctx->last_pos = avio_skip(s->pb, size)) < 0)
            return (int) ctx->last_pos;
        return skip_tlv_packet(pkt);
    }

    if (ctx->cap < size) {
        // Reset cap to 0 before allocating: on failure buf is NULL, and a stale cap == size would
        // make a later call skip the realloc and read into NULL.
        av_freep(&ctx->buf);
        ctx->cap = 0;
        if ((ctx->buf = av_malloc(size)) == NULL)
            return AVERROR(ENOMEM);
        ctx->cap = size;
    }
    if ((err = ffio_read_size(s->pb, ctx->buf, size)) < 0)
        return avio_feof(s->pb) ? AVERROR_EOF : err;
    ctx->last_pos += size;

    switch (header[1]) {
    case IPV4_PACKET:
    case IPV6_PACKET:
        err = mmttlv_read_ip_packet(ctx, s, pkt, ctx->buf, size, header[1]);
        break;
    case HEADER_COMPRESSED_IP_PACKET:
        err = mmttlv_read_compressed_ip_packet(ctx, s, pkt, ctx->buf, size);
        break;
    case TRANSMISSION_CONTROL_PACKET:
        err = mmttlv_read_tlv_si_packet(ctx, s, pkt, ctx->buf, size);
        break;
    default:
        return skip_tlv_packet(pkt);
    }

    if (err >= 0)
        normalize_packet_timestamps(ctx, s, pkt);
    else if (err == AVERROR_INVALIDDATA || err == AVERROR_PATCHWELCOME) {
        av_log(s, AV_LOG_WARNING, "Invalid or unsupported TLV packet; skipping\n");
        return skip_tlv_packet(pkt);
    }
    return err;
}

static int mmttlv_read_header(AVFormatContext *s)
{
    int64_t              pos;
    int64_t              allow = s->probesize;
    struct MMTTLVContext *ctx  = s->priv_data;

    ctx->last_pos = avio_tell(s->pb);
    if (ctx->last_pos < 0)
        return (int) ctx->last_pos;
    ctx->last_pos--; // force resync

    ctx->resync_size = TLV_MAX_PACKET_SIZE;
    ctx->timestamp_origin_us = AV_NOPTS_VALUE;
    ctx->ntp_anchor = 0;
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    if (!s->pb->seekable)
        return 0;

    if ((pos = avio_tell(s->pb)) < 0)
        return (int) pos;

    while (s->nb_streams <= 0 && allow > 0) {
        const int64_t cur = ctx->last_pos;
        const int     err = mmttlv_read_packet(s, NULL);
        if (err < 0)
            return err;
        allow -= ctx->last_pos - cur;
    }

    ctx->last_pos = avio_tell(s->pb);
    if (ctx->last_pos < 0)
        return (int) ctx->last_pos;

    if ((pos = avio_seek(s->pb, pos, SEEK_SET)) < 0)
        return (int) pos;

    return 0;
}

static int mmttlv_read_close(AVFormatContext *ctx)
{
    struct Program       *program;
    struct MMTTLVContext *priv = ctx->priv_data;
    for (program = priv->programs; program != NULL;) {
        struct Program *next = program->next;
        ff_mmtp_parse_close(program->mmtp);
        av_free(program);
        program = next;
    }
    priv->programs = NULL;
    priv->cap      = 0;
    priv->nb_programs = 0;
    priv->nb_raw_flows = 0;
    av_freep(&priv->buf);
    return 0;
}

#if CONFIG_MMTTLV_DEMUXER
const FFInputFormat ff_mmttlv_demuxer = {
    .p.name         = "mmttlv",
    .p.long_name    = NULL_IF_CONFIG_SMALL(
        "MMT protocol over TLV packets (ARIB STD-B60/B32)"),
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT | AVFMT_NO_BYTE_SEEK,
    .priv_data_size = sizeof(struct MMTTLVContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = mmttlv_probe,
    .read_header    = mmttlv_read_header,
    .read_packet    = mmttlv_read_packet,
    .read_close     = mmttlv_read_close,
};
#endif
