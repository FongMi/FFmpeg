/*
 * TTML subtitle decoder
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
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "ass.h"
#include "avcodec.h"
#include "codec_internal.h"

#define TTML_STYLE_NS "http://www.w3.org/ns/ttml#styling"
#define TTML_PARAM_NS "http://www.w3.org/ns/ttml#parameter"
#define ARIB_TT_NS    "http://www.arib.or.jp/ns/arib-ttml/v1_0"

#define TTML_MAX_PACKET_SIZE (4 * 1024 * 1024)
#define TTML_MAX_TREE_DEPTH  64

enum {
    STYLE_FONT          = 1 << 0,
    STYLE_FONT_SIZE     = 1 << 1,
    STYLE_COLOR         = 1 << 2,
    STYLE_BACKGROUND    = 1 << 3,
    STYLE_BOLD          = 1 << 4,
    STYLE_ITALIC        = 1 << 5,
    STYLE_SPACING       = 1 << 6,
    STYLE_ORIGIN        = 1 << 7,
    STYLE_EXTENT        = 1 << 8,
    STYLE_UNDERLINE     = 1 << 9,
    STYLE_STRIKEOUT     = 1 << 10,
    STYLE_OPACITY       = 1 << 11,
    STYLE_TEXT_ALIGN    = 1 << 12,
    STYLE_DISPLAY_ALIGN = 1 << 13,
    STYLE_DIRECTION     = 1 << 14,
    STYLE_WRAP          = 1 << 15,
    STYLE_LAYER         = 1 << 16,
};

#define STYLE_TEXT_MASK (STYLE_FONT | STYLE_FONT_SIZE | STYLE_COLOR | \
                         STYLE_BACKGROUND | STYLE_BOLD | STYLE_ITALIC | \
                         STYLE_SPACING | STYLE_UNDERLINE | STYLE_STRIKEOUT | \
                         STYLE_OPACITY)

#define STYLE_LAYOUT_MASK (STYLE_ORIGIN | STYLE_EXTENT | STYLE_TEXT_ALIGN | \
                           STYLE_DISPLAY_ALIGN | STYLE_DIRECTION | STYLE_WRAP)

typedef enum TTMLLengthUnit {
    TTML_LENGTH_PIXEL,
    TTML_LENGTH_PERCENT,
    TTML_LENGTH_EM,
    TTML_LENGTH_CELL,
    TTML_LENGTH_ROOT_WIDTH,
    TTML_LENGTH_ROOT_HEIGHT,
} TTMLLengthUnit;

typedef struct TTMLLength {
    double value;
    TTMLLengthUnit unit;
} TTMLLength;

enum {
    TTML_ALIGN_START,
    TTML_ALIGN_LEFT,
    TTML_ALIGN_CENTER,
    TTML_ALIGN_RIGHT,
    TTML_ALIGN_END,
};

enum {
    TTML_DISPLAY_BEFORE,
    TTML_DISPLAY_CENTER,
    TTML_DISPLAY_AFTER,
};

enum {
    TTML_DIRECTION_LTR,
    TTML_DIRECTION_RTL,
};

typedef struct TTMLStyle {
    unsigned flags;
    char font[128];
    double font_size_x;
    double font_size_y;
    double spacing;
    double opacity;
    uint32_t color;
    uint32_t background;
    double x, y;
    double width, height;
    int bold;
    int italic;
    int underline;
    int strikeout;
    int text_align;
    int display_align;
    int direction;
    int wrap;
    int layer;
} TTMLStyle;

typedef struct TTMLDecoderContext {
    FFASSDecoderContext ass;
} TTMLDecoderContext;

typedef struct TTMLLayout {
    double width;
    double height;
    double scale_x;
    double scale_y;
    int cell_columns;
    int cell_rows;
    double default_font_size;
} TTMLLayout;

typedef struct TTMLTiming {
    AVRational frame_rate;
    AVRational tick_rate;
    int nominal_frame_rate;
    int sub_frame_rate;
} TTMLTiming;

typedef struct TTMLInterval {
    int64_t begin;
    int64_t end;
} TTMLInterval;

typedef struct TTMLTextState {
    int emitted;
    int pending_space;
} TTMLTextState;

typedef struct TTMLAnimation {
    int64_t show_time;
    int64_t hide_time;
} TTMLAnimation;

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

static const char *skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    return text;
}

static int parse_length_token(const char *text, const char **end,
                              TTMLLength *length)
{
    char *number_end;

    if (!text)
        return AVERROR(EINVAL);
    length->value = strtod(text, &number_end);
    if (number_end == text || !isfinite(length->value))
        return AVERROR_INVALIDDATA;

    if (!strncmp(number_end, "px", 2)) {
        length->unit = TTML_LENGTH_PIXEL;
        number_end += 2;
    } else if (!strncmp(number_end, "em", 2)) {
        length->unit = TTML_LENGTH_EM;
        number_end += 2;
    } else if (!strncmp(number_end, "rw", 2)) {
        length->unit = TTML_LENGTH_ROOT_WIDTH;
        number_end += 2;
    } else if (!strncmp(number_end, "rh", 2)) {
        length->unit = TTML_LENGTH_ROOT_HEIGHT;
        number_end += 2;
    } else if (*number_end == '%') {
        length->unit = TTML_LENGTH_PERCENT;
        number_end++;
    } else if (*number_end == 'c') {
        length->unit = TTML_LENGTH_CELL;
        number_end++;
    } else if (!*number_end || *number_end == ' ' || *number_end == '\t') {
        length->unit = TTML_LENGTH_PIXEL;
    } else {
        return AVERROR_INVALIDDATA;
    }
    if (*number_end && *number_end != ' ' && *number_end != '\t')
        return AVERROR_INVALIDDATA;
    *end = number_end;
    return 0;
}

static int parse_length(const xmlChar *value, TTMLLength *length)
{
    const char *end;
    int ret;

    if (!value)
        return AVERROR(EINVAL);
    ret = parse_length_token((const char *) value, &end, length);
    if (ret < 0)
        return ret;
    return *skip_spaces(end) ? AVERROR_INVALIDDATA : 0;
}

static int parse_length_pair(const xmlChar *value, TTMLLength *first,
                             TTMLLength *second)
{
    const char *end;
    int ret;

    if (!value)
        return AVERROR(EINVAL);
    ret = parse_length_token((const char *) value, &end, first);
    if (ret < 0)
        return ret;
    end = skip_spaces(end);
    if (!*end)
        return AVERROR_INVALIDDATA;
    ret = parse_length_token(end, &end, second);
    if (ret < 0)
        return ret;
    return *skip_spaces(end) ? AVERROR_INVALIDDATA : 0;
}

static int parse_color_function(const char *text, int components,
                                uint32_t *color)
{
    unsigned values[4] = { 0, 0, 0, 255 };
    const char *cursor = text;

    for (int i = 0; i < components; i++) {
        char *end;
        unsigned long component;

        cursor = skip_spaces(cursor);
        errno = 0;
        component = strtoul(cursor, &end, 10);
        if (errno || end == cursor || component > 255)
            return AVERROR_INVALIDDATA;
        values[i] = component;
        cursor = skip_spaces(end);
        if (i + 1 < components) {
            if (*cursor != ',')
                return AVERROR_INVALIDDATA;
        } else if (*cursor != ')')
            return AVERROR_INVALIDDATA;
        cursor++;
    }
    if (*skip_spaces(cursor))
        return AVERROR_INVALIDDATA;

    *color = values[0] << 24 | values[1] << 16 |
             values[2] << 8 | values[3];
    return 0;
}

static int parse_color(const xmlChar *value, uint32_t *color)
{
    static const struct {
        const char *name;
        uint32_t rgba;
    } named_colors[] = {
        { "aqua",        0x00ffffff },
        { "black",       0x000000ff },
        { "blue",        0x0000ffff },
        { "cyan",        0x00ffffff },
        { "fuchsia",     0xff00ffff },
        { "gray",        0x808080ff },
        { "green",       0x008000ff },
        { "lime",        0x00ff00ff },
        { "magenta",     0xff00ffff },
        { "maroon",      0x800000ff },
        { "navy",        0x000080ff },
        { "olive",       0x808000ff },
        { "purple",      0x800080ff },
        { "red",         0xff0000ff },
        { "silver",      0xc0c0c0ff },
        { "teal",        0x008080ff },
        { "transparent", 0x00000000 },
        { "white",       0xffffffff },
        { "yellow",      0xffff00ff },
    };
    const char *text = (const char *) value;
    char *end;
    uint64_t parsed;
    int length;

    if (!value)
        return AVERROR_INVALIDDATA;
    if (!av_strncasecmp(text, "rgb(", 4))
        return parse_color_function(text + 4, 3, color);
    if (!av_strncasecmp(text, "rgba(", 5))
        return parse_color_function(text + 5, 4, color);
    if (value[0] != '#') {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(named_colors); i++) {
            if (!av_strcasecmp(text, named_colors[i].name)) {
                *color = named_colors[i].rgba;
                return 0;
            }
        }
        return AVERROR_INVALIDDATA;
    }

    length = strlen(text + 1);
    if (length != 6 && length != 8)
        return AVERROR_INVALIDDATA;
    for (int i = 1; value[i]; i++)
        if (!av_isxdigit(value[i]))
            return AVERROR_INVALIDDATA;
    parsed = strtoull(text + 1, &end, 16);
    if (*end)
        return AVERROR_INVALIDDATA;
    if (length == 6)
        parsed = (parsed << 8) | 0xff;
    *color = parsed;
    return 0;
}

static int parse_font_size(const xmlChar *value, TTMLLength *horizontal,
                           TTMLLength *vertical)
{
    const char *end;
    int ret;

    if (!value)
        return AVERROR(EINVAL);
    ret = parse_length_token((const char *) value, &end, horizontal);
    if (ret < 0)
        return ret;
    end = skip_spaces(end);
    if (!*end) {
        *vertical = *horizontal;
    } else {
        ret = parse_length_token(end, &end, vertical);
        if (ret < 0 || *skip_spaces(end))
            return AVERROR_INVALIDDATA;
    }
    if (horizontal->value <= 0 || vertical->value <= 0)
        return AVERROR_INVALIDDATA;
    if (horizontal->unit != TTML_LENGTH_PIXEL &&
        horizontal->unit != TTML_LENGTH_PERCENT &&
        horizontal->unit != TTML_LENGTH_EM &&
        horizontal->unit != TTML_LENGTH_CELL)
        return AVERROR_INVALIDDATA;
    if (vertical->unit != TTML_LENGTH_PIXEL &&
        vertical->unit != TTML_LENGTH_PERCENT &&
        vertical->unit != TTML_LENGTH_EM &&
        vertical->unit != TTML_LENGTH_CELL)
        return AVERROR_INVALIDDATA;
    return 0;
}

static double resolve_length(const TTMLLength *length,
                             const TTMLLayout *layout, int horizontal,
                             double em_size)
{
    switch (length->unit) {
    case TTML_LENGTH_PERCENT:
        return length->value * (horizontal ? layout->width : layout->height) /
               100.0;
    case TTML_LENGTH_EM:
        return length->value * em_size;
    case TTML_LENGTH_CELL:
        return length->value * (horizontal ?
               layout->width / layout->cell_columns :
               layout->height / layout->cell_rows);
    case TTML_LENGTH_ROOT_WIDTH:
        return length->value * layout->width / 100.0;
    case TTML_LENGTH_ROOT_HEIGHT:
        return length->value * layout->height / 100.0;
    case TTML_LENGTH_PIXEL:
    default:
        return length->value *
               (horizontal ? layout->scale_x : layout->scale_y);
    }
}

static double resolve_font_length(const TTMLLength *length,
                                  const TTMLLayout *layout, int horizontal,
                                  double font_size)
{
    if (length->unit == TTML_LENGTH_PERCENT)
        return length->value * font_size / 100.0;
    return resolve_length(length, layout, horizontal, font_size);
}

static double get_em_size(const TTMLStyle *style, const TTMLLayout *layout,
                          int horizontal)
{
    if (!(style->flags & STYLE_FONT_SIZE))
        return layout->default_font_size;
    return horizontal ? style->font_size_x : style->font_size_y;
}

static int parse_font_family(const xmlChar *value, char *font, size_t size)
{
    static const struct {
        const char *ttml;
        const char *ass;
    } generic_families[] = {
        { "default",                "sans-serif" },
        { "monospaceSansSerif",     "monospace" },
        { "monospaceSerif",         "monospace" },
        { "proportionalSansSerif",  "sans-serif" },
        { "proportionalSerif",      "serif" },
        { "sansSerif",              "sans-serif" },
    };
    const char *start = skip_spaces((const char *) value);
    const char *end;
    char quote = 0;
    size_t length;

    if (*start == '\'' || *start == '"')
        quote = *start++;
    end = start;
    while (*end && ((quote && *end != quote) || (!quote && *end != ',')))
        end++;
    if (quote && *end != quote)
        return AVERROR_INVALIDDATA;
    if (quote) {
        const char *tail = skip_spaces(end + 1);

        if (*tail && *tail != ',')
            return AVERROR_INVALIDDATA;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    length = end - start;
    if (!length || length >= size)
        return AVERROR_INVALIDDATA;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(generic_families); i++) {
        if (strlen(generic_families[i].ttml) == length &&
            !av_strncasecmp(start, generic_families[i].ttml, length)) {
            av_strlcpy(font, generic_families[i].ass, size);
            return 0;
        }
    }
    memcpy(font, start, length);
    font[length] = 0;
    return 0;
}

static int parse_opacity(const xmlChar *value, double *opacity)
{
    const char *text = (const char *) value;
    char *end;
    double parsed;

    if (!value)
        return AVERROR(EINVAL);
    parsed = strtod(text, &end);
    if (end == text || *end || !isfinite(parsed) || parsed < 0 || parsed > 1)
        return AVERROR_INVALIDDATA;
    *opacity = parsed;
    return 0;
}

static int parse_integer(const xmlChar *value, int *result)
{
    const char *text = (const char *) value;
    char *end;
    long parsed;

    if (!value || !value[0])
        return AVERROR_INVALIDDATA;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX)
        return AVERROR_INVALIDDATA;
    *result = parsed;
    return 0;
}

static void apply_text_decoration(const xmlChar *value, TTMLStyle *style)
{
    char *copy = av_strdup((const char *) value);
    char *save = NULL;
    char *token;

    if (!copy)
        return;
    for (token = av_strtok(copy, " \t\r\n", &save); token;
         token = av_strtok(NULL, " \t\r\n", &save)) {
        if (!av_strcasecmp(token, "none")) {
            style->underline = 0;
            style->strikeout = 0;
            style->flags |= STYLE_UNDERLINE | STYLE_STRIKEOUT;
        } else if (!av_strcasecmp(token, "underline")) {
            style->underline = 1;
            style->flags |= STYLE_UNDERLINE;
        } else if (!av_strcasecmp(token, "noUnderline")) {
            style->underline = 0;
            style->flags |= STYLE_UNDERLINE;
        } else if (!av_strcasecmp(token, "lineThrough")) {
            style->strikeout = 1;
            style->flags |= STYLE_STRIKEOUT;
        } else if (!av_strcasecmp(token, "noLineThrough")) {
            style->strikeout = 0;
            style->flags |= STYLE_STRIKEOUT;
        }
    }
    av_free(copy);
}

static int parse_text_alignment(const xmlChar *value, int *alignment)
{
    /* ASS has no equivalent for TTML justification; use its required fallback. */
    if (!xmlStrcasecmp(value, BAD_CAST "start") ||
        !xmlStrcasecmp(value, BAD_CAST "justify"))
        *alignment = TTML_ALIGN_START;
    else if (!xmlStrcasecmp(value, BAD_CAST "left"))
        *alignment = TTML_ALIGN_LEFT;
    else if (!xmlStrcasecmp(value, BAD_CAST "center"))
        *alignment = TTML_ALIGN_CENTER;
    else if (!xmlStrcasecmp(value, BAD_CAST "right"))
        *alignment = TTML_ALIGN_RIGHT;
    else if (!xmlStrcasecmp(value, BAD_CAST "end"))
        *alignment = TTML_ALIGN_END;
    else
        return AVERROR_INVALIDDATA;
    return 0;
}

static int parse_display_alignment(const xmlChar *value, int *alignment)
{
    if (!xmlStrcasecmp(value, BAD_CAST "before") ||
        !xmlStrcasecmp(value, BAD_CAST "justify"))
        *alignment = TTML_DISPLAY_BEFORE;
    else if (!xmlStrcasecmp(value, BAD_CAST "center"))
        *alignment = TTML_DISPLAY_CENTER;
    else if (!xmlStrcasecmp(value, BAD_CAST "after"))
        *alignment = TTML_DISPLAY_AFTER;
    else
        return AVERROR_INVALIDDATA;
    return 0;
}

static int parse_direction(const xmlChar *value, int *direction)
{
    if (!xmlStrcasecmp(value, BAD_CAST "ltr"))
        *direction = TTML_DIRECTION_LTR;
    else if (!xmlStrcasecmp(value, BAD_CAST "rtl"))
        *direction = TTML_DIRECTION_RTL;
    else
        return AVERROR_INVALIDDATA;
    return 0;
}

static int parse_wrap_option(const xmlChar *value, int *wrap)
{
    if (!xmlStrcasecmp(value, BAD_CAST "wrap"))
        *wrap = 1;
    else if (!xmlStrcasecmp(value, BAD_CAST "noWrap"))
        *wrap = 0;
    else
        return AVERROR_INVALIDDATA;
    return 0;
}

static xmlNode *find_element_by_id(xmlDoc *doc, const char *id)
{
    xmlAttr *attribute = xmlGetID(doc, BAD_CAST id);

    return attribute ? attribute->parent : NULL;
}

static void apply_direct_style(xmlNode *node, TTMLStyle *style,
                               const TTMLLayout *layout)
{
    xmlChar *value;
    TTMLLength first, second;
    uint32_t color;

    value = get_prop(node, "fontFamily", TTML_STYLE_NS);
    if (value) {
        if (parse_font_family(value, style->font, sizeof(style->font)) >= 0)
            style->flags |= STYLE_FONT;
        xmlFree(value);
    }
    value = get_prop(node, "fontSize", TTML_STYLE_NS);
    if (value) {
        if (parse_font_size(value, &first, &second) >= 0) {
            const double base_x = get_em_size(style, layout, 1);
            const double base_y = get_em_size(style, layout, 0);
            const double size_x = resolve_font_length(&first, layout, 1,
                                                      base_x);
            const double size_y = resolve_font_length(&second, layout, 0,
                                                      base_y);

            if (isfinite(size_x) && isfinite(size_y) &&
                size_x > 0 && size_y > 0) {
                style->font_size_x = size_x;
                style->font_size_y = size_y;
                style->flags |= STYLE_FONT_SIZE;
            }
        }
        xmlFree(value);
    }
    value = get_prop(node, "color", TTML_STYLE_NS);
    if (value) {
        if (parse_color(value, &color) >= 0) {
            style->color = color;
            style->flags |= STYLE_COLOR;
        }
        xmlFree(value);
    }
    value = get_prop(node, "backgroundColor", TTML_STYLE_NS);
    if (value) {
        if (parse_color(value, &color) >= 0) {
            style->background = color;
            style->flags |= STYLE_BACKGROUND;
        }
        xmlFree(value);
    }
    value = get_prop(node, "fontWeight", TTML_STYLE_NS);
    if (value) {
        if (!xmlStrcasecmp(value, BAD_CAST "bold")) {
            style->bold = 1;
            style->flags |= STYLE_BOLD;
        } else if (!xmlStrcasecmp(value, BAD_CAST "normal")) {
            style->bold = 0;
            style->flags |= STYLE_BOLD;
        }
        xmlFree(value);
    }
    value = get_prop(node, "fontStyle", TTML_STYLE_NS);
    if (value) {
        if (!xmlStrcasecmp(value, BAD_CAST "italic") ||
            !xmlStrcasecmp(value, BAD_CAST "oblique")) {
            style->italic = 1;
            style->flags |= STYLE_ITALIC;
        } else if (!xmlStrcasecmp(value, BAD_CAST "normal")) {
            style->italic = 0;
            style->flags |= STYLE_ITALIC;
        }
        xmlFree(value);
    }
    value = get_prop(node, "letter-spacing", ARIB_TT_NS);
    if (!value)
        value = get_prop(node, "letterSpacing", TTML_STYLE_NS);
    if (value) {
        if (!xmlStrcasecmp(value, BAD_CAST "normal")) {
            style->spacing = 0;
            style->flags |= STYLE_SPACING;
        } else if (parse_length(value, &first) >= 0) {
            const double em_size = get_em_size(style, layout, 1);
            const double spacing = resolve_font_length(&first, layout, 1,
                                                       em_size);

            if (isfinite(spacing)) {
                style->spacing = spacing;
                style->flags |= STYLE_SPACING;
            }
        }
        xmlFree(value);
    }
    if (!node_is(node, "tt")) {
        value = get_prop(node, "origin", TTML_STYLE_NS);
        if (value) {
            if (parse_length_pair(value, &first, &second) >= 0) {
                const double em_x = get_em_size(style, layout, 1);
                const double em_y = get_em_size(style, layout, 0);
                const double x = resolve_length(&first, layout, 1, em_x);
                const double y = resolve_length(&second, layout, 0, em_y);

                if (isfinite(x) && isfinite(y)) {
                    style->x = x;
                    style->y = y;
                    style->flags |= STYLE_ORIGIN;
                }
            }
            xmlFree(value);
        }
        value = get_prop(node, "extent", TTML_STYLE_NS);
        if (value) {
            if (parse_length_pair(value, &first, &second) >= 0) {
                const double em_x = get_em_size(style, layout, 1);
                const double em_y = get_em_size(style, layout, 0);
                const double width = resolve_length(&first, layout, 1, em_x);
                const double height = resolve_length(&second, layout, 0, em_y);

                if (isfinite(width) && isfinite(height) &&
                    width >= 0 && height >= 0) {
                    style->width = width;
                    style->height = height;
                    style->flags |= STYLE_EXTENT;
                }
            }
            xmlFree(value);
        }
    }
    value = get_prop(node, "textDecoration", TTML_STYLE_NS);
    if (value) {
        apply_text_decoration(value, style);
        xmlFree(value);
    }
    value = get_prop(node, "opacity", TTML_STYLE_NS);
    if (value) {
        if (parse_opacity(value, &style->opacity) >= 0)
            style->flags |= STYLE_OPACITY;
        xmlFree(value);
    }
    value = get_prop(node, "textAlign", TTML_STYLE_NS);
    if (value) {
        if (parse_text_alignment(value, &style->text_align) >= 0)
            style->flags |= STYLE_TEXT_ALIGN;
        xmlFree(value);
    }
    value = get_prop(node, "displayAlign", TTML_STYLE_NS);
    if (value) {
        if (parse_display_alignment(value, &style->display_align) >= 0)
            style->flags |= STYLE_DISPLAY_ALIGN;
        xmlFree(value);
    }
    value = get_prop(node, "direction", TTML_STYLE_NS);
    if (value) {
        if (parse_direction(value, &style->direction) >= 0)
            style->flags |= STYLE_DIRECTION;
        xmlFree(value);
    }
    value = get_prop(node, "wrapOption", TTML_STYLE_NS);
    if (value) {
        if (parse_wrap_option(value, &style->wrap) >= 0)
            style->flags |= STYLE_WRAP;
        xmlFree(value);
    }
    value = get_prop(node, "zIndex", TTML_STYLE_NS);
    if (value) {
        if (!xmlStrcasecmp(value, BAD_CAST "auto")) {
            style->layer = 0;
            style->flags &= ~STYLE_LAYER;
        } else if (parse_integer(value, &style->layer) >= 0) {
            style->flags |= STYLE_LAYER;
        }
        xmlFree(value);
    }
}

static void apply_node_style(xmlDoc *doc, xmlNode *node, TTMLStyle *style,
                             const TTMLLayout *layout, int depth)
{
    xmlChar *references;

    if (!node || depth >= TTML_MAX_TREE_DEPTH)
        return;

    references = get_prop(node, "style", NULL);
    if (references) {
        char *save = NULL;
        char *copy = av_strdup((const char *) references);
        char *reference;

        xmlFree(references);
        if (copy) {
            for (reference = av_strtok(copy, " \t\r\n", &save); reference;
                 reference = av_strtok(NULL, " \t\r\n", &save)) {
                xmlNode *style_node;
                if (*reference == '#')
                    reference++;
                style_node = find_element_by_id(doc, reference);
                if (style_node && style_node != node)
                    apply_node_style(doc, style_node, style, layout,
                                     depth + 1);
            }
        }
        av_free(copy);
    }
    apply_direct_style(node, style, layout);
}

static void apply_element_style(xmlDoc *doc, xmlNode *node, TTMLStyle *style,
                                const TTMLLayout *layout)
{
    const int has_parent_opacity = style->flags & STYLE_OPACITY;
    const double parent_opacity = has_parent_opacity ? style->opacity : 1;

    /* Opacity is not inherited, but nested areas are composited. */
    style->flags &= ~STYLE_OPACITY;
    apply_node_style(doc, node, style, layout, 0);
    if (style->flags & STYLE_OPACITY) {
        style->opacity *= parent_opacity;
    } else if (has_parent_opacity) {
        style->opacity = parent_opacity;
        style->flags |= STYLE_OPACITY;
    }
}

static void resolve_style(xmlDoc *doc, xmlNode *node, TTMLStyle *style,
                          const TTMLLayout *layout)
{
    xmlNode *ancestors[TTML_MAX_TREE_DEPTH];
    xmlChar *region = NULL;
    int count = 0;

    memset(style, 0, sizeof(*style));
    for (xmlNode *cur = node;
         cur && count < (int) FF_ARRAY_ELEMS(ancestors);
         cur = cur->parent) {
        if (cur->type != XML_ELEMENT_NODE)
            continue;
        ancestors[count++] = cur;
        if (!region)
            region = get_prop(cur, "region", NULL);
    }

    if (region) {
        xmlNode *region_node = find_element_by_id(doc, (const char *) region);
        if (region_node)
            apply_element_style(doc, region_node, style, layout);
        xmlFree(region);
    }
    while (count-- > 0)
        apply_element_style(doc, ancestors[count], style, layout);

    if (style->flags & (STYLE_ORIGIN | STYLE_EXTENT)) {
        if (!(style->flags & STYLE_ORIGIN)) {
            style->x = style->y = 0;
            style->flags |= STYLE_ORIGIN;
        }
        if (!(style->flags & STYLE_EXTENT)) {
            style->width  = FFMAX(layout->width - style->x, 0);
            style->height = FFMAX(layout->height - style->y, 0);
            style->flags |= STYLE_EXTENT;
        }
    }
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

static int parse_cell_resolution(const xmlChar *value, int *columns, int *rows)
{
    const char *text = (const char *) value;
    char *end;
    unsigned long first, second;

    if (!value || !value[0])
        return AVERROR_INVALIDDATA;
    errno = 0;
    first = strtoul(text, &end, 10);
    if (errno || end == text || !first || first > INT_MAX ||
        (*end != ' ' && *end != '\t'))
        return AVERROR_INVALIDDATA;
    text = skip_spaces(end);
    errno = 0;
    second = strtoul(text, &end, 10);
    if (errno || end == text || *end || !second || second > INT_MAX)
        return AVERROR_INVALIDDATA;
    *columns = first;
    *rows = second;
    return 0;
}

static void init_layout(const AVCodecContext *avctx, xmlNode *root,
                        TTMLLayout *layout)
{
    TTMLLength horizontal, vertical;
    xmlChar *value;

    layout->width = avctx->width > 0 ? avctx->width : 3840;
    layout->height = avctx->height > 0 ? avctx->height : 2160;
    layout->scale_x = 1;
    layout->scale_y = 1;
    layout->cell_columns = 32;
    layout->cell_rows = 15;
    layout->default_font_size = FFMAX(layout->width / 40, 1);

    value = get_prop(root, "cellResolution", TTML_PARAM_NS);
    if (value) {
        parse_cell_resolution(value, &layout->cell_columns,
                              &layout->cell_rows);
        xmlFree(value);
    }

    value = get_prop(root, "extent", TTML_STYLE_NS);
    if (value) {
        /* Root extent defines the TTML pixel coordinate space. */
        if (parse_length_pair(value, &horizontal, &vertical) >= 0 &&
            horizontal.unit == TTML_LENGTH_PIXEL &&
            vertical.unit == TTML_LENGTH_PIXEL &&
            horizontal.value > 0 && vertical.value > 0) {
            layout->scale_x = layout->width / horizontal.value;
            layout->scale_y = layout->height / vertical.value;
        }
        xmlFree(value);
    }
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

static int parse_timing_parameters(xmlNode *root, TTMLTiming *timing)
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

static int64_t parse_time_expression(const xmlChar *value,
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
    *result = parse_time_expression(value, timing);
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

static int resolve_interval(xmlNode *node, const TTMLTiming *timing,
                            int64_t root_end, TTMLInterval *interval, int depth)
{
    TTMLInterval parent = { 0, root_end };
    int64_t duration, local_begin, local_end, reference;
    int has_begin, has_duration, has_end;
    int sequential = 0;
    int ret;

    if (depth >= TTML_MAX_TREE_DEPTH)
        return AVERROR_INVALIDDATA;

    if (node->parent && node->parent->type == XML_ELEMENT_NODE &&
        !node_is(node->parent, "tt")) {
        ret = resolve_interval(node->parent, timing, root_end,
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
            ret = resolve_interval(sibling, timing, root_end,
                                   &previous, depth + 1);
            if (ret < 0)
                return ret;
            if (previous.end == AV_NOPTS_VALUE)
                return AVERROR_INVALIDDATA;
            reference = previous.end;
        }
    }

    has_begin = get_time(node, "begin", timing, &local_begin);
    if (has_begin < 0)
        return has_begin;
    if ((ret = add_time(reference, has_begin ? local_begin : 0,
                        &interval->begin)) < 0)
        return ret;

    interval->end = parent.end;
    has_end = get_time(node, "end", timing, &local_end);
    if (has_end < 0)
        return has_end;
    if (has_end) {
        if ((ret = add_time(reference, local_end, &local_end)) < 0)
            return ret;
        interval->end = local_end;
        if (parent.end != AV_NOPTS_VALUE)
            interval->end = FFMIN(interval->end, parent.end);
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

static unsigned get_ass_alpha(double opacity)
{
    return 0xff - (unsigned) lrint(av_clipd(opacity, 0, 1) * 0xff);
}

static void append_color_value(AVBPrint *buf, int index, uint32_t rgba)
{
    const unsigned red   = rgba >> 24;
    const unsigned green = (rgba >> 16) & 0xff;
    const unsigned blue  = (rgba >> 8) & 0xff;

    av_bprintf(buf, "\\%dc&H%02X%02X%02X&", index, blue, green, red);
}

static void append_color_alpha(AVBPrint *buf, int index, uint32_t rgba,
                               double opacity)
{
    const unsigned alpha = get_ass_alpha((rgba & 0xff) / 255.0 * opacity);

    av_bprintf(buf, "\\%da&H%02X&", index, alpha);
}

static void append_style_alpha(AVBPrint *buf, const TTMLStyle *style)
{
    const double opacity = style->flags & STYLE_OPACITY ? style->opacity : 1;

    av_bprintf(buf, "\\alpha&H%02X&", get_ass_alpha(opacity));
    if (style->flags & STYLE_COLOR)
        append_color_alpha(buf, 1, style->color, opacity);
    if (style->flags & STYLE_BACKGROUND)
        append_color_alpha(buf, 3, style->background, opacity);
}

static int text_styles_equal(const TTMLStyle *first, const TTMLStyle *second)
{
    const unsigned first_flags = first->flags & STYLE_TEXT_MASK;
    const unsigned second_flags = second->flags & STYLE_TEXT_MASK;

    if (first_flags != second_flags)
        return 0;
    if ((first_flags & STYLE_FONT) && strcmp(first->font, second->font))
        return 0;
    if ((first_flags & STYLE_FONT_SIZE) &&
        (first->font_size_x != second->font_size_x ||
         first->font_size_y != second->font_size_y))
        return 0;
    if ((first_flags & STYLE_COLOR) && first->color != second->color)
        return 0;
    if ((first_flags & STYLE_BACKGROUND) &&
        first->background != second->background)
        return 0;
    if ((first_flags & STYLE_BOLD) && first->bold != second->bold)
        return 0;
    if ((first_flags & STYLE_ITALIC) && first->italic != second->italic)
        return 0;
    if ((first_flags & STYLE_SPACING) && first->spacing != second->spacing)
        return 0;
    if ((first_flags & STYLE_UNDERLINE) &&
        first->underline != second->underline)
        return 0;
    if ((first_flags & STYLE_STRIKEOUT) &&
        first->strikeout != second->strikeout)
        return 0;
    if ((first_flags & STYLE_OPACITY) && first->opacity != second->opacity)
        return 0;
    return 1;
}

static int get_ass_alignment(const TTMLStyle *style,
                             double *horizontal_offset,
                             double *vertical_offset)
{
    int horizontal;
    int vertical = style->display_align;

    switch (style->text_align) {
    case TTML_ALIGN_LEFT:
        horizontal = 0;
        break;
    case TTML_ALIGN_CENTER:
        horizontal = 1;
        break;
    case TTML_ALIGN_RIGHT:
        horizontal = 2;
        break;
    case TTML_ALIGN_END:
        horizontal = style->direction == TTML_DIRECTION_RTL ? 0 : 2;
        break;
    case TTML_ALIGN_START:
    default:
        horizontal = style->direction == TTML_DIRECTION_RTL ? 2 : 0;
        break;
    }

    *horizontal_offset = horizontal / 2.0;
    *vertical_offset = vertical / 2.0;
    return (2 - vertical) * 3 + horizontal + 1;
}

static void append_style_tags(AVBPrint *buf, const TTMLStyle *style,
                              const TTMLAnimation *animation,
                              int include_position, int reset)
{
    unsigned flags = style->flags & STYLE_TEXT_MASK;
    const double opacity = style->flags & STYLE_OPACITY ? style->opacity : 1;
    const int show = animation && animation->show_time >= 0;
    const int hide = animation && animation->hide_time >= 0;

    if (include_position)
        flags |= style->flags & STYLE_LAYOUT_MASK;
    if (!flags && !reset && !show && !hide)
        return;

    av_bprintf(buf, reset ? "{\\r" : "{");
    if ((style->flags & STYLE_FONT) &&
        !strpbrk(style->font, "{},\\\r\n"))
        av_bprintf(buf, "\\fn%s", style->font);
    if (style->flags & STYLE_FONT_SIZE) {
        av_bprintf(buf, "\\fs%.2f", style->font_size_y);
        av_bprintf(buf, "\\fscx%.2f",
                   style->font_size_x / style->font_size_y * 100);
    }
    if ((style->flags & STYLE_OPACITY) && !show)
        av_bprintf(buf, "\\alpha&H%02X&", get_ass_alpha(opacity));
    if (style->flags & STYLE_COLOR) {
        append_color_value(buf, 1, style->color);
        if (!show)
            append_color_alpha(buf, 1, style->color, opacity);
    }
    if (style->flags & STYLE_BACKGROUND) {
        append_color_value(buf, 3, style->background);
        if (!show)
            append_color_alpha(buf, 3, style->background, opacity);
    }
    if (style->flags & STYLE_BOLD)
        av_bprintf(buf, "\\b%d", style->bold);
    if (style->flags & STYLE_ITALIC)
        av_bprintf(buf, "\\i%d", style->italic);
    if (style->flags & STYLE_SPACING)
        av_bprintf(buf, "\\fsp%.2f", style->spacing);
    if (style->flags & STYLE_UNDERLINE)
        av_bprintf(buf, "\\u%d", style->underline);
    if (style->flags & STYLE_STRIKEOUT)
        av_bprintf(buf, "\\s%d", style->strikeout);
    if (include_position &&
        (style->flags & (STYLE_ORIGIN | STYLE_TEXT_ALIGN |
                         STYLE_DISPLAY_ALIGN | STYLE_DIRECTION))) {
        double horizontal_offset, vertical_offset;
        const int alignment = get_ass_alignment(style, &horizontal_offset,
                                                &vertical_offset);

        av_bprintf(buf, "\\an%d", alignment);
        if (style->flags & STYLE_ORIGIN)
            av_bprintf(buf, "\\pos(%.2f,%.2f)",
                       style->x + style->width * horizontal_offset,
                       style->y + style->height * vertical_offset);
    }
    if (include_position && (style->flags & STYLE_WRAP))
        av_bprintf(buf, "\\q%d", style->wrap ? 0 : 2);
    if (include_position &&
        (style->flags & (STYLE_ORIGIN | STYLE_EXTENT)) ==
        (STYLE_ORIGIN | STYLE_EXTENT))
        av_bprintf(buf, "\\clip(%.2f,%.2f,%.2f,%.2f)",
                   style->x, style->y,
                   style->x + style->width, style->y + style->height);
    /* Every styled run needs the paragraph visibility animation. */
    if (show) {
        av_bprintf(buf, "\\alpha&HFF&\\t(%"PRId64",%"PRId64",",
                   animation->show_time, animation->show_time);
        append_style_alpha(buf, style);
        av_bprintf(buf, ")");
    }
    if (hide)
        av_bprintf(buf, "\\t(%"PRId64",%"PRId64",\\alpha&HFF&)",
                   animation->hide_time, animation->hide_time);
    av_bprintf(buf, "}");
}

static int is_xml_whitespace(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int get_xml_space(xmlNode *node, int inherited)
{
    xmlChar *value = xmlGetNsProp(node, BAD_CAST "space", XML_XML_NAMESPACE);

    if (!value)
        return inherited;
    if (!xmlStrcmp(value, BAD_CAST "preserve"))
        inherited = 1;
    else if (!xmlStrcmp(value, BAD_CAST "default"))
        inherited = 0;
    xmlFree(value);
    return inherited;
}

static void append_text(AVBPrint *buf, const xmlChar *text, int preserve,
                        TTMLTextState *state)
{
    const char *start = (const char *) text;
    const char *cursor = start;

    if (preserve) {
        if (state->pending_space && state->emitted)
            av_bprint_chars(buf, ' ', 1);
        state->pending_space = 0;
        if (*cursor) {
            ff_ass_bprint_text_event(buf, cursor, strlen(cursor), NULL, 0);
            state->emitted = 1;
        }
        return;
    }

    while (*cursor) {
        if (is_xml_whitespace(*cursor)) {
            state->pending_space = state->emitted;
            cursor++;
            continue;
        }

        start = cursor;
        while (*cursor && !is_xml_whitespace(*cursor))
            cursor++;
        if (state->pending_space && state->emitted)
            av_bprint_chars(buf, ' ', 1);
        state->pending_space = 0;
        ff_ass_bprint_text_event(buf, start, cursor - start, NULL, 0);
        state->emitted = 1;
    }
}

static void append_text_children(xmlDoc *doc, xmlNode *parent, AVBPrint *buf,
                                 const TTMLStyle *parent_style,
                                 const TTMLLayout *layout,
                                 const TTMLAnimation *animation,
                                 TTMLTextState *text_state,
                                 int preserve_space, int depth)
{
    if (depth >= TTML_MAX_TREE_DEPTH)
        return;

    for (xmlNode *node = parent->children; node; node = node->next) {
        if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
            append_text(buf, node->content, preserve_space, text_state);
        } else if (node_is(node, "br")) {
            text_state->pending_space = 0;
            text_state->emitted = 0;
            av_bprintf(buf, "\\N");
        } else if (node->type == XML_ELEMENT_NODE) {
            TTMLStyle child_style = *parent_style;
            int child_space = get_xml_space(node, preserve_space);
            int style_changed;

            apply_element_style(doc, node, &child_style, layout);
            style_changed = !text_styles_equal(&child_style, parent_style);
            if (style_changed)
                append_style_tags(buf, &child_style, animation, 0, 0);
            append_text_children(doc, node, buf, &child_style, layout,
                                 animation, text_state, child_space, depth + 1);
            if (style_changed)
                append_style_tags(buf, parent_style, animation, 0, 1);
        }
    }
}

static int collect_timing(xmlNode *node, const TTMLTiming *timing,
                          int64_t *minimum, int64_t *maximum,
                          int *paragraphs, int *has_indefinite, int depth)
{
    int ret;

    if (depth >= TTML_MAX_TREE_DEPTH)
        return AVERROR_INVALIDDATA;
    for (; node; node = node->next) {
        if (node_is(node, "p")) {
            TTMLInterval interval;

            ret = resolve_interval(node, timing, AV_NOPTS_VALUE,
                                   &interval, 0);
            if (ret < 0)
                return ret;
            *minimum = FFMIN(*minimum, interval.begin);
            if (interval.end == AV_NOPTS_VALUE)
                *has_indefinite = 1;
            else
                *maximum = FFMAX(*maximum, interval.end);
            (*paragraphs)++;
        }
        ret = collect_timing(node->children, timing, minimum, maximum,
                             paragraphs, has_indefinite, depth + 1);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int render_paragraphs(xmlDoc *doc, xmlNode *node, AVSubtitle *sub,
                             TTMLDecoderContext *ctx, const TTMLTiming *timing,
                             const TTMLLayout *layout,
                             int64_t minimum, int64_t maximum, int depth)
{
    int ret;

    if (depth >= TTML_MAX_TREE_DEPTH)
        return 0;
    for (; node; node = node->next) {
        if (node_is(node, "p")) {
            TTMLAnimation animation;
            TTMLInterval interval;
            TTMLTextState text_state = { 0 };
            TTMLStyle style;
            AVBPrint buf;
            int preserve_space = get_xml_space(node, 0);

            ret = resolve_interval(node, timing, maximum, &interval, 0);
            if (ret < 0)
                return ret;
            resolve_style(doc, node, &style, layout);
            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
            animation.show_time = interval.begin > minimum ?
                                  interval.begin - minimum : -1;
            animation.hide_time = interval.end < maximum ?
                                  interval.end - minimum : -1;
            append_style_tags(&buf, &style, &animation, 1, 0);
            append_text_children(doc, node, &buf, &style, layout,
                                 &animation, &text_state, preserve_space, 0);

            if (!av_bprint_is_complete(&buf)) {
                av_bprint_finalize(&buf, NULL);
                return AVERROR(ENOMEM);
            }
            ret = ff_ass_add_rect(sub, buf.str, ctx->ass.readorder++,
                                  style.flags & STYLE_LAYER ? style.layer : 0,
                                  NULL, NULL);
            av_bprint_finalize(&buf, NULL);
            if (ret < 0)
                return ret;
        }
        ret = render_paragraphs(doc, node->children, sub, ctx, timing, layout,
                                minimum, maximum, depth + 1);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static av_cold int ttml_init(AVCodecContext *avctx)
{
    const int width  = avctx->width  > 0 ? avctx->width  : 3840;
    const int height = avctx->height > 0 ? avctx->height : 2160;
    const int font_size = FFMAX(width / 40, 1);

    return ff_ass_subtitle_header_full(avctx, width, height,
                                       "sans-serif", font_size,
                                       0xffffff, 0xffffff,
                                       0xff000000, 0xff000000,
                                       0, 0, 0, 3, 7);
}

static int ttml_decode_frame(AVCodecContext *avctx, AVSubtitle *sub,
                             int *got_sub_ptr, const AVPacket *avpkt)
{
    TTMLDecoderContext *ctx = avctx->priv_data;
    TTMLLayout layout;
    TTMLTiming timing;
    xmlDoc *doc;
    xmlNode *root;
    int has_indefinite = 0;
    int paragraphs = 0;
    int64_t minimum = INT64_MAX;
    int64_t maximum = 0;
    int ret;

    if (!avpkt->data || avpkt->size <= 0 ||
        avpkt->size > TTML_MAX_PACKET_SIZE)
        return AVERROR_INVALIDDATA;

    doc = xmlReadMemory((const char *) avpkt->data, avpkt->size,
                        NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                    XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    if (!doc) {
        av_log(avctx, AV_LOG_WARNING, "Invalid TTML document\n");
        return AVERROR_INVALIDDATA;
    }
    root = xmlDocGetRootElement(doc);
    if (!node_is(root, "tt")) {
        xmlFreeDoc(doc);
        return AVERROR_INVALIDDATA;
    }

    init_layout(avctx, root, &layout);
    ret = parse_timing_parameters(root, &timing);
    if (ret < 0) {
        xmlFreeDoc(doc);
        return ret;
    }
    ret = collect_timing(root, &timing, &minimum, &maximum, &paragraphs,
                         &has_indefinite, 0);
    if (ret < 0) {
        xmlFreeDoc(doc);
        return ret;
    }
    if (!paragraphs) {
        xmlFreeDoc(doc);
        *got_sub_ptr = 0;
        return avpkt->size;
    }
    if (minimum == INT64_MAX)
        minimum = 0;
    if (has_indefinite || maximum <= minimum) {
        int64_t fallback;

        if (avpkt->duration > 0 && avctx->pkt_timebase.num > 0 &&
            avctx->pkt_timebase.den > 0) {
            int64_t duration = av_rescale_q(avpkt->duration,
                                            avctx->pkt_timebase,
                                            (AVRational){ 1, 1000 });
            duration = FFMAX(duration, 1);
            if (duration > UINT32_MAX - minimum) {
                xmlFreeDoc(doc);
                return AVERROR(EOVERFLOW);
            }
            fallback = minimum + duration;
        } else {
            fallback = minimum + 5000;
        }
        maximum = FFMAX(maximum, fallback);
    }
    if (minimum > UINT32_MAX || maximum > UINT32_MAX) {
        xmlFreeDoc(doc);
        return AVERROR(EOVERFLOW);
    }

    ret = render_paragraphs(doc, root, sub, ctx, &timing, &layout,
                            minimum, maximum, 0);
    xmlFreeDoc(doc);
    if (ret < 0)
        return ret;

    sub->start_display_time = minimum;
    sub->end_display_time   = maximum;
    *got_sub_ptr = sub->num_rects > 0;
    return avpkt->size;
}

const FFCodec ff_ttml_decoder = {
    .p.name         = "ttml",
    CODEC_LONG_NAME("Timed Text Markup Language subtitle"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_TTML,
    FF_CODEC_DECODE_SUB_CB(ttml_decode_frame),
    .init           = ttml_init,
    .flush          = ff_ass_decoder_flush,
    .priv_data_size = sizeof(TTMLDecoderContext),
};
