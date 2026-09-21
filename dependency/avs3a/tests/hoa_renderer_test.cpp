/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "hoa_renderer.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <vector>

static std::vector<int16_t> render(Avs3HoaRenderer *renderer, std::vector<int16_t> pcm,
                                   int channels, int samples)
{
    char error[128] = {};
    assert(avs3_hoa_renderer_process(renderer, pcm.data(), samples, channels,
                                     error, sizeof(error)) == 0);
    pcm.resize(samples * 2);
    return pcm;
}

int main()
{
    Avs3HoaRenderer *renderer = nullptr;
    assert(avs3_hoa_renderer_create(&renderer, 4) == 0);

    // ACN/N3D first-order coefficients for sources at +/-30 degrees azimuth.
    auto left = render(renderer, {282, 244, 0, 423}, 4, 1);
    auto right = render(renderer, {282, -244, 0, 423}, 4, 1);
    assert(left[0] > left[1]);
    assert(right[1] > right[0]);
    assert(left[0] == right[1] && left[1] == right[0]);

    // Rendering into the same buffer must preserve later input samples.
    auto both = render(renderer, {282, 244, 0, 423, 282, -244, 0, 423}, 4, 2);
    assert(both == std::vector<int16_t>({left[0], left[1], right[0], right[1]}));
    assert(avs3_hoa_renderer_process(renderer, both.data(), 2, 2, nullptr, 0) == -EINVAL);
    avs3_hoa_renderer_destroy(renderer);

    for (int channels : {9, 16}) {
        assert(avs3_hoa_renderer_create(&renderer, channels) == 0);
        std::vector<int16_t> pcm(channels);
        pcm[0] = 1000;
        auto stereo = render(renderer, pcm, channels, 1);
        assert(stereo[0] != 0 && stereo[0] == stereo[1]);
        avs3_hoa_renderer_destroy(renderer);
    }
    assert(avs3_hoa_renderer_create(&renderer, 3) == -EINVAL && !renderer);
}
