/*
 * Check height-channel contributions and normalization when downmixing.
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

#include <math.h>
#include <stdio.h>

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/macros.h"
#include "libswresample/swresample.h"

static void check_matrix(enum AVMatrixEncoding encoding)
{
    const AVChannelLayout input = AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK;
    const AVChannelLayout outputs[] = {
        AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK,
        AV_CHANNEL_LAYOUT_7POINT1,
        AV_CHANNEL_LAYOUT_5POINT1,
        AV_CHANNEL_LAYOUT_5POINT1_BACK,
        AV_CHANNEL_LAYOUT_STEREO,
        AV_CHANNEL_LAYOUT_MONO,
        AV_CHANNEL_LAYOUT_MASK(3, AV_CH_LAYOUT_STEREO | AV_CH_TOP_BACK_RIGHT),
    };
    const enum AVChannel height[] = {
        AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT,
        AV_CHAN_TOP_BACK_LEFT, AV_CHAN_TOP_BACK_RIGHT,
    };

    for (int n = 0; n < FF_ARRAY_ELEMS(outputs); n++) {
        double matrix[64][64] = { 0 };
        int ret = swr_build_matrix2(&input, &outputs[n], sqrt(0.5), sqrt(0.5),
                                   0.0, 1.0, 1.0, &matrix[0][0], 64,
                                   encoding, NULL);
        av_assert0(ret >= 0);
        for (int h = 0; h < FF_ARRAY_ELEMS(height); h++) {
            int in = av_channel_layout_index_from_channel(&input, height[h]);
            int direct = av_channel_layout_index_from_channel(&outputs[n], height[h]);
            double contribution = 0;
            for (int out = 0; out < outputs[n].nb_channels; out++) {
                contribution += fabs(matrix[out][in]);
                if (direct >= 0 && out != direct)
                    av_assert0(matrix[out][in] == 0);
            }
            av_assert0(contribution > 0);
        }

        for (int right = 0; right < 2; right++) {
            int in = av_channel_layout_index_from_channel(
                &input, right ? AV_CHAN_TOP_BACK_RIGHT : AV_CHAN_TOP_BACK_LEFT);
            int out = av_channel_layout_index_from_channel(
                &outputs[n], right ? AV_CHAN_BACK_RIGHT : AV_CHAN_BACK_LEFT);
            if (out < 0)
                out = av_channel_layout_index_from_channel(
                    &outputs[n], right ? AV_CHAN_SIDE_RIGHT : AV_CHAN_SIDE_LEFT);
            if (out >= 0 && outputs[n].nb_channels < input.nb_channels) {
                for (int j = 0; j < outputs[n].nb_channels; j++)
                    av_assert0(j == out ? matrix[j][in] > 0 : matrix[j][in] == 0);
            }
        }
        for (int out = 0; out < outputs[n].nb_channels; out++) {
            double sum = 0;
            for (int in = 0; in < input.nb_channels; in++)
                sum += fabs(matrix[out][in]);
            av_assert0(sum <= 1.0 + 1e-12);
        }
        if (outputs[n].nb_channels == 2 && encoding != AV_MATRIX_ENCODING_NONE) {
            for (int h = 2; h < FF_ARRAY_ELEMS(height); h++) {
                int in = av_channel_layout_index_from_channel(&input, height[h]);
                av_assert0(matrix[0][in] < 0 && matrix[1][in] > 0);
            }
        }
    }
}

int main(void)
{
    check_matrix(AV_MATRIX_ENCODING_NONE);
    check_matrix(AV_MATRIX_ENCODING_DOLBY);
    check_matrix(AV_MATRIX_ENCODING_DPLII);
    puts("height channel rematrix ok");
    return 0;
}
