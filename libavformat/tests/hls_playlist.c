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

#include "libavformat/hls.c"

static int check_playlist(const char *line, int position,
                          int allow_repeated, int valid_cue)
{
    AVFormatContext *format = avformat_alloc_context();
    HLSContext hls = { .ctx = format };
    FFIOContext io;
    char *manifest = av_asprintf(
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "%s\n"
        "#EXTINF:4,\n"
        "before.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4,\n"
        "%s\n"
        "ad.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4,\n"
        "after.ts\n"
        "#EXT-X-ENDLIST\n"
        "%s\n", position == 0 ? line : "",
                  position == 1 ? line : "",
                  position == 2 ? line : "");
    int ret = 1;

    if (!format || !manifest)
        goto end;
    format->priv_data = &hls;
    ffio_init_read_context(&io, (const uint8_t *)manifest, strlen(manifest));
    if (parse_playlist(&hls, "http://test.invalid/index.m3u8", NULL, &io.pub) < 0)
        goto end;
    if (hls.n_playlists != 1 || !hls.playlists[0]->finished ||
        hls.playlists[0]->n_segments != 3 || format->duration != 12000000 ||
        !hls.playlists[0]->segments[1]->discontinuity ||
        !hls.playlists[0]->segments[2]->discontinuity ||
        hls.playlists[0]->allow_repeated_ad_blocks != allow_repeated ||
        hls.playlists[0]->valid_cue != valid_cue) {
        fprintf(stderr, "HLS comment/tag handling mismatch: '%s'\n", line);
        goto end;
    }
    ret = 0;

end:
    free_playlist_list(&hls);
    free_variant_list(&hls);
    free_rendition_list(&hls);
    if (format)
        format->priv_data = NULL;
    avformat_free_context(format);
    av_free(manifest);
    return ret;
}

static int check_byte_ranges(void)
{
    struct segment segments[6] = {0};
    struct segment *entries[6];
    struct segment init = { .url_offset = 8, .size = 4 };
    struct playlist pls = {
        .start_seq_no = 100, .cur_seq_no = 100,
        .n_segments = 6, .segments = entries,
    };

    for (int i = 0; i < 6; i++) {
        entries[i] = &segments[i];
        segments[i].url = "http://test.invalid/segments.ts";
        segments[i].url_offset = 128 + 4 * i;
        segments[i].size = 4;
        segments[i].key_type = KEY_NONE;
    }
    if (range_end(&pls, entries[0]) != 152 ||
        range_end(&pls, entries[1]) != 152 ||
        !segment_continues(entries[0], entries[1], 136) ||
        segment_continues(entries[0], entries[1], 135))
        return 1;
    segments[1].ad_removed = segments[2].ad_removed = 1;
    segments[5].ad_removed = 1;
    if (next_segment(&pls) != entries[3] ||
        range_end(&pls, entries[0]) != 132 ||
        range_end(&pls, entries[3]) != 148 ||
        range_end(&pls, &init) != 12)
        return 1;
    pls.cur_seq_no = 103;
    if (range_end(&pls, entries[3]) != 148 ||
        !segment_continues(entries[3], entries[4], 148) ||
        segment_continues(entries[3], NULL, 148))
        return 1;
    segments[4].key_type = KEY_AES_128;
    if (segment_continues(entries[3], entries[4], 148) ||
        range_end(&pls, entries[3]) != 144)
        return 1;
    segments[4].key_type = KEY_NONE;
    segments[4].init_section = &init;
    if (segment_continues(entries[3], entries[4], 148))
        return 1;
    segments[4].init_section = NULL;
    segments[4].url = "http://test.invalid/other.ts";
    if (segment_continues(entries[3], entries[4], 148))
        return 1;
    return 0;
}

static int check_external_opening(void)
{
    static const char manifest[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:3\n#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:3,\n/other/hls/0.ts\n"
        "#EXTINF:3,\n/other/hls/1.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:3,\n2.ts\n#EXTINF:3,\n3.ts\n#EXTINF:3,\n4.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXT-X-ENDLIST\n";
    AVFormatContext *format = avformat_alloc_context();
    HLSContext hls = { .ctx = format, .cur_timestamp = AV_NOPTS_VALUE };
    FFIOContext io;
    struct playlist *pls;
    int ret = 1;

    if (!format)
        return 1;
    format->priv_data = &hls;
    ffio_init_read_context(&io, (const uint8_t *)manifest, strlen(manifest));
    if (parse_playlist(&hls, "https://test.invalid/movie/hls/index.m3u8", NULL, &io.pub) < 0 ||
        hls.n_playlists != 1 || format->duration != 15000000)
        goto end;
    pls = hls.playlists[0];
    /* Disabled means the original playlist and first request are preserved. */
    apply_ad_detection(&hls);
    if (pls->segments[0]->ad_removed || next_segment(pls) != pls->segments[1])
        goto end;
    hls.hls_adblock = 1;
    apply_ad_detection(&hls);
    pls->cur_seq_no = select_cur_seq_no(&hls, pls);
    /* read_header reserves the first nonremoved segment before opening TS I/O. */
    while (pls->cur_seq_no < pls->start_seq_no + pls->n_segments &&
           pls->segments[pls->cur_seq_no - pls->start_seq_no]->ad_removed)
        pls->cur_seq_no++;
    if (format->duration != 9000000 ||
        pls->cur_seq_no != pls->start_seq_no + 2 ||
        !pls->segments[0]->ad_removed || !pls->segments[1]->ad_removed ||
        pls->segments[2]->ad_removed || next_segment(pls) != pls->segments[3])
        goto end;
    ret = 0;
end:
    free_playlist_list(&hls);
    free_variant_list(&hls);
    free_rendition_list(&hls);
    format->priv_data = NULL;
    avformat_free_context(format);
    return ret;
}

#if HAVE_THREADS
static int check_ad_frontier(void)
{
    struct segment segments[6] = {0};
    struct segment *entries[6];
    struct playlist pls = {
        .start_seq_no = 100, .cur_seq_no = 100,
        .n_segments = 6, .segments = entries,
    };
    struct playlist *playlists[] = { &pls };
    AVFormatContext *format = avformat_alloc_context();
    HLSContext hls = {
        .ctx = format, .first_timestamp = 0, .playlists = playlists,
        .n_playlists = 1, .ad_probe_started = 1,
    };
    uint8_t confirmed[6] = {0}, covered[6] = {0};
    int ret = 1;

    if (!format)
        return 1;
    if (ff_mutex_init(&hls.ad_probe_mutex, NULL))
        goto end;
    hls.ad_probe_confirmed = confirmed;
    for (int i = 0; i < 6; i++) {
        entries[i] = &segments[i];
        segments[i].duration = 4000000;
    }
    format->duration = 24000000;
    /* Even a manually supplied leading confirmation is rejected. */
    ad_probe_publish(&hls, covered, 0, 3, 0);
    if (covered[0] || confirmed[0])
        goto destroy;
    ad_probe_publish(&hls, covered, 1, 3, 0);
    if (!covered[1] || !confirmed[1])
        goto destroy;
    /* Playback may advance between confirmation and application. */
    pls.cur_seq_no = 101;
    apply_probed_ads(&hls);
    if (segments[0].ad_removed || segments[1].ad_removed ||
        segments[2].ad_removed || format->duration != 24000000 ||
        pls.has_discontinuity)
        goto destroy;
    /* Backward seeks cannot make selected prefixes removable again. */
    pls.cur_seq_no = 100;
    memset(covered, 0, sizeof(covered));
    ad_probe_publish(&hls, covered, 1, 3, 0);
    apply_probed_ads(&hls);
    if (covered[1] || segments[1].ad_removed || segments[2].ad_removed ||
        format->duration != 24000000)
        goto destroy;
    /* Interior windows remain atomic when playback enters their range. */
    memset(covered, 0, sizeof(covered));
    ad_probe_publish(&hls, covered, 3, 5, 0);
    if (!covered[3])
        goto destroy;
    pls.cur_seq_no = 103;
    apply_probed_ads(&hls);
    if (segments[3].ad_removed || segments[4].ad_removed ||
        format->duration != 24000000)
        goto destroy;
    /* A future interior window is still removed once, including after backward seeks. */
    memset(covered, 0, sizeof(covered));
    ad_probe_publish(&hls, covered, 4, 5, 0);
    apply_probed_ads(&hls);
    if (!segments[4].ad_removed || segments[5].ad_removed ||
        format->duration != 20000000 || !pls.has_discontinuity)
        goto destroy;
    memset(covered, 0, sizeof(covered));
    pls.cur_seq_no = 100;
    ad_probe_publish(&hls, covered, 4, 5, 0);
    apply_probed_ads(&hls);
    if (format->duration != 20000000)
        goto destroy;
    ret = 0;
destroy:
    ff_mutex_destroy(&hls.ad_probe_mutex);
end:
    avformat_free_context(format);
    return ret;
}
#endif

int main(void)
{
    static const struct {
        const char *line;
        int allow_repeated;
        int valid_cue;
    } cases[] = {
        { "",                             1, 1 },
        { "#",                            1, 1 },
        { "# ordinary comment",           1, 1 },
        { "# ppvod-ad-injected v1",        1, 1 },
        { "# comment with #EXT text",     1, 1 },
        { "#ext-lowercase-comment",       1, 1 },
        { "#EXT-X-VERSION:3",             1, 1 },
        { "#EXT-X-INDEPENDENT-SEGMENTS",  1, 1 },
        { "#EXT-X-UNKNOWN:1",             0, 1 },
        { "#EXT-X-SKIP:SKIPPED-SEGMENTS=1", 0, 0 },
    };

    if (check_byte_ranges()) {
        fprintf(stderr, "HLS byte-range/ad-boundary mismatch\n");
        return 1;
    }
    if (check_external_opening()) {
        fprintf(stderr, "HLS external opening startup mismatch\n");
        return 1;
    }
#if HAVE_THREADS
    if (check_ad_frontier()) {
        fprintf(stderr, "HLS interior-ad frontier mismatch\n");
        return 1;
    }
#endif
    for (int i = 0; i < FF_ARRAY_ELEMS(cases); i++)
        for (int position = 0; position < 3; position++)
            if (check_playlist(cases[i].line, position,
                               cases[i].allow_repeated, cases[i].valid_cue))
                return 1;
    return 0;
}
