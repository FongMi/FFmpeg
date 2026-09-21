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

#define ffio_open_whitelist test_open_whitelist
#define ff_hls_ad_probe test_hls_ad_probe
#include "libavformat/hls_ad_probe.c"

static int open_calls;

int test_open_whitelist(AVIOContext **s, const char *url, int flags,
                        const AVIOInterruptCB *int_cb, AVDictionary **options,
                        const char *whitelist, const char *blacklist)
{
    open_calls++;
    return AVERROR_BUG;
}

int main(void)
{
    AVDictionary *options = NULL;
    FFHLSAdProbeResult result = { 0 };
    char *value;
    int ret;
    int64_t pts[] = {1400000, 1440000, 1480000, 1520000, 1560000, 1600000};
    FFHLSAdTiming timing = probe_timing(pts, FF_ARRAY_ELEMS(pts));

    if (timing.first_pts != 1400000 || timing.last_pts != 1600000 ||
        timing.sample_duration != 40000 || probe_timing(pts, 5).sample_duration)
        return 1;
    pts[3] += 2000;
    if (probe_timing(pts, FF_ARRAY_ELEMS(pts)).sample_duration)
        return 1;
    pts[3] = pts[2];
    if (probe_timing(pts, FF_ARRAY_ELEMS(pts)).sample_duration)
        return 1;
    pts[2] = 1520000;
    pts[3] = 1480000;
    if (probe_timing(pts, FF_ARRAY_ELEMS(pts)).sample_duration)
        return 1;
    pts[2] = 1480000;
    pts[3] = 1520000;
    for (unsigned int i = 0; i < FF_ARRAY_ELEMS(pts); i++)
        pts[i] -= 1500000;
    timing = probe_timing(pts, FF_ARRAY_ELEMS(pts));
    if (timing.first_pts != -100000 || timing.last_pts != 100000 ||
        timing.sample_duration != 40000)
        return 1;

    value = av_malloc(12 * 1024 * 1024);
    if (!value)
        return 1;
    memset(value, 'x', 12 * 1024 * 1024 - 1);
    value[12 * 1024 * 1024 - 1] = 0;
    ret = av_dict_set(&options, "headers", value, AV_DICT_DONT_STRDUP_VAL);
    if (ret < 0)
        return 1;

    /* Initial probe allocations fit; duplicating this header does not. */
    av_max_alloc(10 * 1024 * 1024);
    ret = test_hls_ad_probe("http://test.invalid/segment.ts", options, NULL,
                            NULL, NULL, INT64_MAX, &result);
    av_max_alloc(SIZE_MAX);
    av_dict_free(&options);

    if (ret != AVERROR(ENOMEM) || open_calls) {
        fprintf(stderr, "HLS probe ignored option-copy failure: ret=%d, opens=%d\n",
                ret, open_calls);
        return 1;
    }
    return 0;
}
