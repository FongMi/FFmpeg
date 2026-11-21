/*
 * TTML subtitle demuxer
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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>

#include "libavcodec/ttml_timing.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

typedef struct TTMLCue {
    xmlNode *paragraph;
    TTMLInterval interval;
    int64_t max_end;
    int order;
} TTMLCue;

typedef struct TTMLDemuxContext {
    xmlDoc *document;
    TTMLCue *cues;
    unsigned allocated;
    int nb_cues;
    int next;
} TTMLDemuxContext;

typedef struct TTMLProbeContext {
    xmlParserCtxt *parser;
    int match;
} TTMLProbeContext;

static int node_is(const xmlNode *node, const char *name)
{
    return node && node->type == XML_ELEMENT_NODE && node->ns &&
           !xmlStrcmp(node->ns->href, BAD_CAST TTML_NS) &&
           !xmlStrcmp(node->name, BAD_CAST name);
}

static void probe_root(void *opaque, const xmlChar *name,
                       const xmlChar *prefix, const xmlChar *uri,
                       int nb_namespaces, const xmlChar **namespaces,
                       int nb_attributes, int nb_defaulted,
                       const xmlChar **attributes)
{
    TTMLProbeContext *ctx = opaque;

    ctx->match = !xmlStrcmp(name, BAD_CAST "tt") &&
                 !xmlStrcmp(uri, BAD_CAST TTML_NS);
    xmlStopParser(ctx->parser);
}

static int ttml_probe(const AVProbeData *probe)
{
    xmlSAXHandler handler = {
        .initialized = XML_SAX2_MAGIC,
        .startElementNs = probe_root,
    };
    TTMLProbeContext ctx = { 0 };

    ctx.parser = xmlCreatePushParserCtxt(&handler, &ctx, NULL, 0, NULL);
    if (!ctx.parser)
        return 0;
    xmlCtxtUseOptions(ctx.parser, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                 XML_PARSE_NOWARNING);
    xmlParseChunk(ctx.parser, (const char *)probe->buf, probe->buf_size, 0);
    xmlFreeParserCtxt(ctx.parser);
    return ctx.match ? AVPROBE_SCORE_MAX : 0;
}

static int collect_cue(void *opaque, xmlNode *node,
                       const TTMLInterval *interval)
{
    TTMLDemuxContext *ctx = opaque;
    TTMLCue *cues;

    if (interval->begin == interval->end)
        return 0;
    cues = av_fast_realloc(ctx->cues, &ctx->allocated,
                           (ctx->nb_cues + 1) * sizeof(*cues));
    if (!cues)
        return AVERROR(ENOMEM);
    ctx->cues = cues;
    cues[ctx->nb_cues] = (TTMLCue) {
        .paragraph = node, .interval = *interval, .order = ctx->nb_cues,
    };
    ctx->nb_cues++;
    return 0;
}

static int compare_cue(const void *a, const void *b)
{
    const TTMLCue *first = a, *second = b;
    int compare = FFDIFFSIGN(first->interval.begin, second->interval.begin);

    return compare ? compare : FFDIFFSIGN(first->order, second->order);
}

static int ttml_read_header(AVFormatContext *s)
{
    TTMLDemuxContext *ctx = s->priv_data;
    TTMLTiming timing;
    AVBPrint data;
    xmlNode *root;
    AVStream *st;
    int64_t max_end = 0;
    int ret;

    av_bprint_init(&data, 0, AV_BPRINT_SIZE_UNLIMITED);
    ret = avio_read_to_bprint(s->pb, &data, TTML_MAX_PACKET_SIZE + 1);
    if (ret >= 0) {
        if (data.len > TTML_MAX_PACKET_SIZE)
            ret = AVERROR_INVALIDDATA;
        else
            ctx->document = xmlReadMemory(data.str, data.len, NULL, NULL,
                                          XML_PARSE_NONET | XML_PARSE_NOERROR |
                                          XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    }
    av_bprint_finalize(&data, NULL);
    if (ret < 0)
        return ret;
    if (!ctx->document)
        return AVERROR_INVALIDDATA;

    root = xmlDocGetRootElement(ctx->document);
    if (!node_is(root, "tt"))
        return AVERROR_INVALIDDATA;
    if ((ret = avpriv_ttml_parse_timing_parameters(root, &timing)) < 0 ||
        (ret = avpriv_ttml_walk_paragraphs(root, &timing, collect_cue, ctx)) < 0)
        return ret;
    if (ctx->nb_cues)
        qsort(ctx->cues, ctx->nb_cues, sizeof(*ctx->cues), compare_cue);
    for (int i = 0; i < ctx->nb_cues; i++) {
        TTMLCue *cue = &ctx->cues[i];
        int64_t end = cue->interval.end;

        max_end = end == AV_NOPTS_VALUE ? INT64_MAX : FFMAX(max_end, end);
        cue->max_end = max_end;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    avpriv_set_pts_info(st, 64, 1, 1000);
    st->start_time = 0;
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_TTML;
    if (ctx->nb_cues && max_end != INT64_MAX)
        st->duration = max_end;
    return 0;
}

static xmlNode *copy_child(xmlNode *parent, xmlNode *source, int extended)
{
    xmlNode *child = xmlDocCopyNode(source, parent->doc, extended);

    if (!child)
        return NULL;
    if (!xmlAddChild(parent, child)) {
        xmlFreeNode(child);
        return NULL;
    }
    return child;
}

static int write_cue(xmlDoc *document, const TTMLCue *cue, AVPacket *pkt)
{
    xmlNode *path[TTML_MAX_TREE_DEPTH];
    xmlNode *root = xmlDocGetRootElement(document);
    xmlNode *parent, *child, *node;
    xmlDoc *sample;
    xmlChar *text = NULL;
    char duration[32];
    int depth = 0, size = 0, ret = AVERROR(ENOMEM);

    for (node = cue->paragraph; node != root; node = node->parent)
        path[depth++] = node;
    sample = xmlNewDoc(BAD_CAST "1.0");
    if (!sample)
        return ret;
    parent = xmlDocCopyNode(root, sample, 2);
    if (!parent)
        goto end;
    xmlDocSetRootElement(sample, parent);
    for (node = root->children; node; node = node->next) {
        if (!node_is(node, "head"))
            continue;
        if (!copy_child(parent, node, 1))
            goto end;
    }

    /* Preserve inherited styling while rebasing the paragraph to its packet PTS. */
    while (depth) {
        node = path[--depth];
        child = copy_child(parent, node, depth ? 2 : 1);
        if (!child)
            goto end;
        parent = child;
        xmlUnsetProp(parent, BAD_CAST "begin");
        xmlUnsetProp(parent, BAD_CAST "end");
        xmlUnsetProp(parent, BAD_CAST "dur");
        xmlUnsetProp(parent, BAD_CAST "timeContainer");
    }
    if (cue->interval.end != AV_NOPTS_VALUE) {
        snprintf(duration, sizeof(duration), "%"PRId64"ms",
                 cue->interval.end - cue->interval.begin);
        if (!xmlSetProp(parent, BAD_CAST "dur", BAD_CAST duration))
            goto end;
    }
    if (xmlReconciliateNs(sample, xmlDocGetRootElement(sample)) < 0)
        goto end;
    xmlDocDumpMemoryEnc(sample, &text, &size, "UTF-8");
    if (!text)
        goto end;
    if (size > TTML_MAX_PACKET_SIZE) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    ret = av_new_packet(pkt, size);
    if (ret >= 0)
        memcpy(pkt->data, text, size);

end:
    xmlFree(text);
    xmlFreeDoc(sample);
    return ret;
}

static int ttml_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    TTMLDemuxContext *ctx = s->priv_data;
    const TTMLCue *cue;
    int ret;

    if (ctx->next == ctx->nb_cues)
        return AVERROR_EOF;
    cue = &ctx->cues[ctx->next];
    if ((ret = write_cue(ctx->document, cue, pkt)) < 0)
        return ret;
    pkt->pts = pkt->dts = cue->interval.begin;
    pkt->flags |= AV_PKT_FLAG_KEY;
    if (cue->interval.end != AV_NOPTS_VALUE)
        pkt->duration = cue->interval.end - cue->interval.begin;
    ctx->next++;
    return 0;
}

static int ttml_read_seek(AVFormatContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    TTMLDemuxContext *ctx = s->priv_data;
    int left = 0, right = ctx->nb_cues;

    if (flags & (AVSEEK_FLAG_BYTE | AVSEEK_FLAG_FRAME))
        return AVERROR(ENOSYS);
    /* Retain any paragraph that is still active at the new playback time. */
    while (left < right) {
        int mid = left + (right - left) / 2;

        if (ctx->cues[mid].max_end <= timestamp)
            left = mid + 1;
        else
            right = mid;
    }
    ctx->next = left;
    return 0;
}

static int ttml_read_close(AVFormatContext *s)
{
    TTMLDemuxContext *ctx = s->priv_data;

    xmlFreeDoc(ctx->document);
    av_freep(&ctx->cues);
    return 0;
}

const FFInputFormat ff_ttml_demuxer = {
    .p.name         = "ttml",
    .p.long_name    = NULL_IF_CONFIG_SMALL("TTML subtitle"),
    .p.extensions   = "ttml,dfxp",
    .p.mime_type    = "application/ttml+xml",
    .p.flags        = AVFMT_NO_BYTE_SEEK,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .priv_data_size = sizeof(TTMLDemuxContext),
    .read_probe     = ttml_probe,
    .read_header    = ttml_read_header,
    .read_packet    = ttml_read_packet,
    .read_seek      = ttml_read_seek,
    .read_close     = ttml_read_close,
};
