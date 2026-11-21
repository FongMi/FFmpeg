/*
 * TTML timing
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "ttml_timing.h"

static int node_is(const xmlNode *node, const char *name)
{
    return node && node->type == XML_ELEMENT_NODE &&
           !xmlStrcmp(node->name, BAD_CAST name);
}

static int is_timed_text_node(const xmlNode *node)
{
    return node_is(node, "body") || node_is(node, "div") ||
           node_is(node, "p") || node_is(node, "span");
}

static xmlChar *get_prop(const xmlNode *node, const char *name,
                         const char *namespace)
{
    xmlChar *value = NULL;

    if (namespace)
        value = xmlGetNsProp((xmlNode *) node, BAD_CAST name,
                             BAD_CAST namespace);
    if (!value)
        value = xmlGetProp((xmlNode *) node, BAD_CAST name);
    return value;
}

static int64_t milliseconds_from_seconds(double seconds)
{
    if (!isfinite(seconds) || seconds < 0 ||
        seconds > UINT32_MAX / 1000.0)
        return AV_NOPTS_VALUE;
    return llrint(seconds * 1000.0);
}

static int parse_positive_int(const xmlChar *value, int *result)
{
    char *end;
    unsigned long parsed;

    if (!value || !value[0])
        return AVERROR_INVALIDDATA;
    errno = 0;
    parsed = strtoul((const char *) value, &end, 10);
    if (errno || *end || !parsed || parsed > INT_MAX)
        return AVERROR_INVALIDDATA;
    *result = parsed;
    return 0;
}

static int parse_frame_rate_multiplier(const xmlChar *value,
                                       int *numerator, int *denominator)
{
    const char *text = (const char *) value;
    char *end;
    unsigned long first, second;

    if (!value || !value[0])
        return AVERROR_INVALIDDATA;

    errno = 0;
    first = strtoul(text, &end, 10);
    if (errno || end == text || (*end != ' ' && *end != '\t') ||
        !first || first > INT_MAX)
        return AVERROR_INVALIDDATA;
    while (*end == ' ' || *end == '\t')
        end++;

    text = end;
    errno = 0;
    second = strtoul(text, &end, 10);
    if (errno || end == text || *end || !second || second > INT_MAX)
        return AVERROR_INVALIDDATA;

    *numerator = first;
    *denominator = second;
    return 0;
}

int avpriv_ttml_parse_timing_parameters(xmlNode *root, TTMLTiming *timing)
{
    xmlChar *value;
    int denominator = 1;
    int frame_rate = 30;
    int frame_rate_specified = 0;
    int numerator = 1;
    int ret;

    timing->sub_frame_rate = 1;

    value = get_prop(root, "timeBase", TTML_PARAM_NS);
    if (value) {
        ret = !xmlStrcmp(value, BAD_CAST "media") ? 0 : AVERROR_PATCHWELCOME;
        xmlFree(value);
        if (ret < 0)
            return ret;
    }

    value = get_prop(root, "frameRate", TTML_PARAM_NS);
    if (value) {
        ret = parse_positive_int(value, &frame_rate);
        xmlFree(value);
        if (ret < 0)
            return ret;
        frame_rate_specified = 1;
    }

    value = get_prop(root, "frameRateMultiplier", TTML_PARAM_NS);
    if (value) {
        ret = parse_frame_rate_multiplier(value, &numerator, &denominator);
        xmlFree(value);
        if (ret < 0)
            return ret;
    }

    timing->nominal_frame_rate = frame_rate;
    av_reduce(&timing->frame_rate.num, &timing->frame_rate.den,
              (int64_t) frame_rate * numerator, denominator, INT_MAX);

    value = get_prop(root, "subFrameRate", TTML_PARAM_NS);
    if (value) {
        ret = parse_positive_int(value, &timing->sub_frame_rate);
        xmlFree(value);
        if (ret < 0)
            return ret;
    }

    value = get_prop(root, "tickRate", TTML_PARAM_NS);
    if (value) {
        int tick_rate;

        ret = parse_positive_int(value, &tick_rate);
        xmlFree(value);
        if (ret < 0)
            return ret;
        timing->tick_rate = (AVRational) { tick_rate, 1 };
    } else if (frame_rate_specified) {
        av_reduce(&timing->tick_rate.num, &timing->tick_rate.den,
                  (int64_t) timing->frame_rate.num * timing->sub_frame_rate,
                  timing->frame_rate.den, INT_MAX);
    } else {
        timing->tick_rate = (AVRational) { 1, 1 };
    }

    return 0;
}

static int64_t milliseconds_from_rate(double count, AVRational rate)
{
    if (rate.num <= 0 || rate.den <= 0)
        return AV_NOPTS_VALUE;
    return milliseconds_from_seconds(count * rate.den / rate.num);
}

static int has_bounded_decimal_runs(const char *text)
{
    int digits = 0;

    for (; *text; text++) {
        if (*text >= '0' && *text <= '9') {
            if (++digits > 9)
                return 0;
        } else {
            digits = 0;
        }
    }
    return 1;
}

int64_t avpriv_ttml_parse_time_expression(const xmlChar *value,
                                     const TTMLTiming *timing)
{
    const char *text = (const char *) value;
    char *end;
    double seconds;
    int consumed, frames, hours, minutes, sub_frames, whole_seconds;

    if (!value || strlen(text) > 64 || !has_bounded_decimal_runs(text))
        return AV_NOPTS_VALUE;
    if (sscanf(text, "%d:%d:%lf%n", &hours, &minutes, &seconds,
               &consumed) == 3 && !text[consumed]) {
        if (hours < 0 || minutes < 0 || minutes >= 60 ||
            seconds < 0 || seconds >= 60)
            return AV_NOPTS_VALUE;
        return milliseconds_from_seconds(hours * 3600.0 +
                                         minutes * 60.0 + seconds);
    }

    sub_frames = 0;
    if ((sscanf(text, "%d:%d:%d:%d.%d%n", &hours, &minutes, &whole_seconds,
                &frames, &sub_frames, &consumed) == 5 ||
         sscanf(text, "%d:%d:%d:%d%n", &hours, &minutes, &whole_seconds,
                &frames, &consumed) == 4) &&
        !text[consumed]) {
        if (hours < 0 || minutes < 0 || minutes >= 60 ||
            whole_seconds < 0 || whole_seconds >= 60 ||
            frames < 0 || frames >= timing->nominal_frame_rate ||
            sub_frames < 0 || sub_frames >= timing->sub_frame_rate)
            return AV_NOPTS_VALUE;
        seconds = hours * 3600.0 + minutes * 60.0 + whole_seconds;
        seconds += (frames + sub_frames / (double) timing->sub_frame_rate) *
                   timing->frame_rate.den / timing->frame_rate.num;
        return milliseconds_from_seconds(seconds);
    }

    seconds = strtod(text, &end);
    if (end == text || !isfinite(seconds) || seconds < 0)
        return AV_NOPTS_VALUE;
    if (!strcmp(end, "ms")) {
        if (seconds > UINT32_MAX)
            return AV_NOPTS_VALUE;
        return llrint(seconds);
    }
    if (!strcmp(end, "s"))
        return milliseconds_from_seconds(seconds);
    if (!strcmp(end, "m"))
        return milliseconds_from_seconds(seconds * 60.0);
    if (!strcmp(end, "h"))
        return milliseconds_from_seconds(seconds * 3600.0);
    if (!strcmp(end, "f"))
        return milliseconds_from_rate(seconds, timing->frame_rate);
    if (!strcmp(end, "t"))
        return milliseconds_from_rate(seconds, timing->tick_rate);
    return AV_NOPTS_VALUE;
}

static int get_time(xmlNode *node, const char *name, const TTMLTiming *timing,
                    int64_t *result)
{
    xmlChar *value = get_prop(node, name, NULL);

    if (!value)
        return 0;
    *result = avpriv_ttml_parse_time_expression(value, timing);
    xmlFree(value);
    return *result == AV_NOPTS_VALUE ? AVERROR_INVALIDDATA : 1;
}

static int add_time(int64_t first, int64_t second, int64_t *result)
{
    if (first < 0 || second < 0 || first > UINT32_MAX - second)
        return AVERROR(EOVERFLOW);
    *result = first + second;
    return 0;
}

static int get_time_container(xmlNode *node, int *sequential)
{
    xmlChar *value = get_prop(node, "timeContainer", NULL);

    *sequential = 0;
    if (!value)
        return 0;
    if (!xmlStrcmp(value, BAD_CAST "seq"))
        *sequential = 1;
    else if (xmlStrcmp(value, BAD_CAST "par")) {
        xmlFree(value);
        return AVERROR_INVALIDDATA;
    }
    xmlFree(value);
    return 0;
}

static int resolve_at_reference(xmlNode *node, const TTMLTiming *timing,
                                const TTMLInterval *parent, int64_t reference,
                                TTMLInterval *interval)
{
    int64_t duration, local_begin, local_end;
    int has_begin, has_duration, has_end;
    int ret;

    has_begin = get_time(node, "begin", timing, &local_begin);
    if (has_begin < 0)
        return has_begin;
    if ((ret = add_time(reference, has_begin ? local_begin : 0,
                        &interval->begin)) < 0)
        return ret;

    interval->end = parent->end;
    has_end = get_time(node, "end", timing, &local_end);
    if (has_end < 0)
        return has_end;
    if (has_end) {
        if ((ret = add_time(reference, local_end, &local_end)) < 0)
            return ret;
        interval->end = local_end;
        if (parent->end != AV_NOPTS_VALUE)
            interval->end = FFMIN(interval->end, parent->end);
    }

    has_duration = get_time(node, "dur", timing, &duration);
    if (has_duration < 0)
        return has_duration;
    if (has_duration) {
        if ((ret = add_time(interval->begin, duration, &duration)) < 0)
            return ret;
        if (interval->end == AV_NOPTS_VALUE)
            interval->end = duration;
        else
            interval->end = FFMIN(interval->end, duration);
    }

    return interval->end != AV_NOPTS_VALUE && interval->end < interval->begin ?
           AVERROR_INVALIDDATA : 0;
}

int avpriv_ttml_resolve_interval(xmlNode *node, const TTMLTiming *timing,
                            int64_t root_end, TTMLInterval *interval, int depth)
{
    TTMLInterval parent = { 0, root_end };
    int64_t reference;
    int sequential = 0;
    int ret;

    if (depth >= TTML_MAX_TREE_DEPTH)
        return AVERROR_INVALIDDATA;

    if (node->parent && node->parent->type == XML_ELEMENT_NODE &&
        !node_is(node->parent, "tt")) {
        ret = avpriv_ttml_resolve_interval(node->parent, timing, root_end,
                               &parent, depth + 1);
        if (ret < 0)
            return ret;
    }
    reference = parent.begin;

    if (node->parent && node->parent->type == XML_ELEMENT_NODE) {
        ret = get_time_container(node->parent, &sequential);
        if (ret < 0)
            return ret;
    }
    if (sequential) {
        for (xmlNode *sibling = node->parent->children;
             sibling && sibling != node; sibling = sibling->next) {
            TTMLInterval previous;

            if (!is_timed_text_node(sibling))
                continue;
            ret = resolve_at_reference(sibling, timing, &parent,
                                       reference, &previous);
            if (ret < 0)
                return ret;
            if (previous.end == AV_NOPTS_VALUE)
                return AVERROR_INVALIDDATA;
            reference = previous.end;
        }
    }

    return resolve_at_reference(node, timing, &parent, reference, interval);
}

static int walk_paragraphs(xmlNode *parent, const TTMLTiming *timing,
                           const TTMLInterval *parent_interval, int depth,
                           int (*visit)(void *, xmlNode *, const TTMLInterval *),
                           void *opaque)
{
    int64_t reference = parent_interval->begin;
    int sequential, ret;

    if (depth >= TTML_MAX_TREE_DEPTH)
        return AVERROR_INVALIDDATA;
    if ((ret = get_time_container(parent, &sequential)) < 0)
        return ret;

    for (xmlNode *node = parent->children; node; node = node->next) {
        TTMLInterval interval;

        if (!is_timed_text_node(node) || !node->ns ||
            xmlStrcmp(node->ns->href, BAD_CAST TTML_NS))
            continue;
        if (reference == AV_NOPTS_VALUE)
            return AVERROR_INVALIDDATA;
        ret = resolve_at_reference(node, timing, parent_interval,
                                   reference, &interval);
        if (ret < 0)
            return ret;
        if (node_is(node, "p"))
            ret = visit(opaque, node, &interval);
        else if (node_is(node, "body") || node_is(node, "div"))
            ret = walk_paragraphs(node, timing, &interval, depth + 1,
                                  visit, opaque);
        if (ret < 0)
            return ret;
        if (sequential)
            reference = interval.end;
    }
    return 0;
}

int avpriv_ttml_walk_paragraphs(xmlNode *root, const TTMLTiming *timing,
                                int (*visit)(void *, xmlNode *,
                                             const TTMLInterval *),
                                void *opaque)
{
    static const TTMLInterval document = { 0, AV_NOPTS_VALUE };

    return walk_paragraphs(root, timing, &document, 0, visit, opaque);
}
