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

#include <inttypes.h>
#include <stdio.h>

#include "libavcodec/ttmldec.c"

static const struct {
    const char *value;
    int64_t expected;
} time_cases[] = {
    { "00:00:01.500", 1500 },
    { "1:02:03.250", 3723250 },
    { "1500ms", 1500 },
    { "1.5s", 1500 },
    { "2m", 120000 },
    { "0.5h", 1800000 },
    { "30f", 1000 },
    { "2t", 2000 },
    { "00:00:01:15", 1500 },
    { "00:00:01.5junk", AV_NOPTS_VALUE },
    { "00:60:00", AV_NOPTS_VALUE },
    { "999999999999999999:00:00", AV_NOPTS_VALUE },
    { "1s ", AV_NOPTS_VALUE },
    { "-1s", AV_NOPTS_VALUE },
    { "1", AV_NOPTS_VALUE },
};

static const TTMLTiming default_timing = {
    .frame_rate        = { 30, 1 },
    .tick_rate         = { 1, 1 },
    .nominal_frame_rate = 30,
    .sub_frame_rate    = 1,
};

static int decode_document(const char *document, AVSubtitle *sub, int *got_sub)
{
    TTMLDecoderContext context = { 0 };
    AVCodecContext avctx = { .priv_data = &context };
    AVPacket packet = {
        .data = (uint8_t *) document,
        .size = strlen(document),
    };

    return ttml_decode_frame(&avctx, sub, got_sub, &packet);
}

static int test_document(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:tts='http://www.w3.org/ns/ttml#styling'"
        " xmlns:arib='http://www.arib.or.jp/ns/arib-ttml/v1_0'>"
        "<head><styling><style xml:id='s' tts:fontFamily='Test'"
        " tts:fontSize='48px' tts:color='#112233'"
        " arib:letter-spacing='2px'/></styling>"
        "<layout><region xml:id='r' tts:origin='10px 20px'"
        " tts:extent='100px 50px'/></layout></head>"
        "<body region='r'><div><p begin='1s' end='2.5s' style='s'>"
        "A &amp; B<br/>C</p></div></body></tt>";
    static const char *const expected[] = {
        "\\fnTest",
        "\\fs48.00",
        "\\1c&H332211&",
        "\\fsp2.00",
        "\\pos(10.00,20.00)",
        "\\clip(10.00,20.00,110.00,70.00)",
        "A & B\\NC",
    };
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;
    size_t i;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        sub.start_display_time != 1000 || sub.end_display_time != 2500 ||
        !sub.rects[0]->ass) {
        fprintf(stderr, "TTML document did not produce the expected event\n");
        avsubtitle_free(&sub);
        return 1;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(expected); i++) {
        if (!strstr(sub.rects[0]->ass, expected[i])) {
            fprintf(stderr, "TTML event is missing: %s\n", expected[i]);
            avsubtitle_free(&sub);
            return 1;
        }
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_relative_timing_and_whitespace(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:ttp='http://www.w3.org/ns/ttml#parameter'"
        " ttp:frameRate='30' ttp:frameRateMultiplier='1000 1001'"
        " ttp:subFrameRate='2' ttp:tickRate='100'>"
        "<body begin='10s' dur='5s'><div begin='1s'>"
        "<p begin='30f' dur='50t'><span>Hello</span> <span>world</span>"
        "<br/><span xml:space='preserve'>A  B</span></p>"
        "</div></body></tt>";
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        sub.start_display_time != 12001 || sub.end_display_time != 12501 ||
        !sub.rects[0]->ass ||
        !strstr(sub.rects[0]->ass, "Hello world\\NA  B")) {
        fprintf(stderr, "TTML relative timing or whitespace handling failed\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_sequential_timing(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'>"
        "<body><div timeContainer='seq'>"
        "<metadata dur='20s'/>"
        "<p dur='1s'>First</p><p dur='2s'>Second</p>"
        "</div></body></tt>";
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 2 ||
        sub.start_display_time != 0 || sub.end_display_time != 3000) {
        fprintf(stderr, "TTML sequential timing failed\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_end_and_duration(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'>"
        "<body><div><p begin='1s' end='5s' dur='2s'>Short</p>"
        "</div></body></tt>";
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        sub.start_display_time != 1000 || sub.end_display_time != 3000) {
        fprintf(stderr, "TTML end and duration handling failed\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_style_reset(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:tts='http://www.w3.org/ns/ttml#styling'>"
        "<body><div><p begin='0s' dur='1s'>"
        "<span tts:fontWeight='bold'>Bold</span>Plain"
        "</p></div></body></tt>";
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        !sub.rects[0]->ass ||
        !strstr(sub.rects[0]->ass, "{\\b1}Bold{\\r}Plain")) {
        fprintf(stderr, "TTML nested style was not reset\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_ass_style_mapping(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:tts='http://www.w3.org/ns/ttml#styling'"
        " xmlns:ttp='http://www.w3.org/ns/ttml#parameter'"
        " tts:extent='1920px 1080px' ttp:cellResolution='40 20'>"
        "<head><styling><style xml:id='s'"
        " tts:fontFamily='&quot;Fancy Font&quot;, sans-serif'"
        " tts:fontSize='50% 150%' tts:color='rgba(1, 2, 3, 128)'"
        " tts:backgroundColor='transparent' tts:opacity='0.5'"
        " tts:fontWeight='BOLD' tts:fontStyle='ITALIC'"
        " tts:textDecoration='underline lineThrough'"
        " tts:wrapOption='noWrap'/></styling>"
        "<layout><region xml:id='r' style='s' tts:origin='25% 50%'"
        " tts:extent='50% 25%' tts:textAlign='center'"
        " tts:displayAlign='after' tts:zIndex='7'/></layout></head>"
        "<body region='r'><div><p begin='0s' dur='1s'>"
        "<span tts:color='rgb(10, 20, 30)'>RGB</span>"
        "<span tts:color='YeLLoW' tts:textDecoration='none'"
        " tts:opacity='1'>Named</span>"
        "</p></div></body></tt>";
    static const char *const expected[] = {
        "0,7,Default",
        "\\fnFancy Font",
        "\\fs144.00\\fscx33.33",
        "\\alpha&H7F&",
        "\\1c&H030201&\\1a&HBF&",
        "\\3c&H000000&\\3a&HFF&",
        "\\b1\\i1",
        "\\u1\\s1",
        "\\an2\\pos(1920.00,1620.00)",
        "\\q2",
        "\\clip(960.00,1080.00,2880.00,1620.00)",
        "\\1c&H1E140A&",
        "\\1c&H00FFFF&",
        "\\u0\\s0",
    };
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;
    size_t i;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        !sub.rects[0]->ass) {
        fprintf(stderr, "TTML ASS style mapping produced no event\n");
        avsubtitle_free(&sub);
        return 1;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(expected); i++) {
        if (!strstr(sub.rects[0]->ass, expected[i])) {
            fprintf(stderr, "TTML ASS event is missing: %s\n", expected[i]);
            avsubtitle_free(&sub);
            return 1;
        }
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_relative_layout_units(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:tts='http://www.w3.org/ns/ttml#styling'"
        " xmlns:ttp='http://www.w3.org/ns/ttml#parameter'"
        " ttp:cellResolution='40 20'>"
        "<head><layout><region xml:id='r' tts:origin='1c 2c'"
        " tts:extent='25rw 10rh' tts:direction='rtl'"
        " tts:textAlign='end' tts:displayAlign='center'/></layout></head>"
        "<body region='r'><div><p begin='0s' dur='1s'>Cell</p>"
        "</div></body></tt>";
    AVSubtitle sub = { 0 };
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 1 ||
        !sub.rects[0]->ass ||
        !strstr(sub.rects[0]->ass, "\\an4\\pos(96.00,324.00)") ||
        !strstr(sub.rects[0]->ass,
                "\\clip(96.00,216.00,1056.00,432.00)")) {
        fprintf(stderr, "TTML relative layout unit mapping failed\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

static int test_styled_timing(void)
{
    static const char document[] =
        "<tt xmlns='http://www.w3.org/ns/ttml'"
        " xmlns:tts='http://www.w3.org/ns/ttml#styling'>"
        "<body><div><p begin='0s' dur='1s' tts:color='#01020380'"
        " tts:opacity='0.5'>First<span tts:opacity='0.5'>Nested</span></p>"
        "<p begin='1s' dur='1s' tts:color='#01020380'"
        " tts:opacity='0.5'>Second</p></div></body></tt>";
    AVSubtitle sub = { 0 };
    const char *first_hide;
    const char *first_text;
    const char *nested_alpha;
    const char *nested_color_alpha;
    const char *nested_text;
    const char *second_show;
    const char *second_text;
    int got_sub = 0;
    int ret;

    ret = decode_document(document, &sub, &got_sub);
    if (ret != sizeof(document) - 1 || !got_sub || sub.num_rects != 2 ||
        !sub.rects[0]->ass || !sub.rects[1]->ass) {
        fprintf(stderr, "TTML styled timing produced no events\n");
        avsubtitle_free(&sub);
        return 1;
    }
    first_hide = strstr(sub.rects[0]->ass,
                        "\\t(1000,1000,\\alpha&HFF&)");
    first_text = strstr(sub.rects[0]->ass, "First");
    nested_alpha = strstr(sub.rects[0]->ass, "\\alpha&HBF&");
    nested_color_alpha = strstr(sub.rects[0]->ass, "\\1a&HDF&");
    nested_text = strstr(sub.rects[0]->ass, "Nested");
    second_show = strstr(sub.rects[1]->ass,
                         "\\alpha&HFF&\\t(1000,1000,"
                         "\\alpha&H7F&\\1a&HBF&)");
    second_text = strstr(sub.rects[1]->ass, "Second");
    if (!first_hide || !first_text || first_hide > first_text ||
        !nested_alpha || !nested_color_alpha || !nested_text ||
        nested_alpha > nested_text || nested_color_alpha > nested_text ||
        !second_show || !second_text || second_show > second_text) {
        fprintf(stderr, "TTML style alpha was not preserved across timing\n");
        avsubtitle_free(&sub);
        return 1;
    }
    avsubtitle_free(&sub);
    return 0;
}

int main(void)
{
    size_t i;

    for (i = 0; i < FF_ARRAY_ELEMS(time_cases); i++) {
        int64_t actual = parse_time_expression(BAD_CAST time_cases[i].value,
                                               &default_timing);

        if (actual != time_cases[i].expected) {
            fprintf(stderr, "parse_time_expression(%s): got %"PRId64
                    ", expected %"PRId64"\n", time_cases[i].value,
                    actual, time_cases[i].expected);
            return 1;
        }
    }
    return test_document() ||
           test_relative_timing_and_whitespace() ||
           test_sequential_timing() ||
           test_end_and_duration() ||
           test_style_reset() ||
           test_ass_style_mapping() ||
           test_relative_layout_units() ||
           test_styled_timing();
}
