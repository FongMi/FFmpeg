/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "hoa_renderer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <vector>

#include <ear/ear.hpp>

struct Avs3HoaRenderer {
    int input_channels;
    std::vector<std::vector<double>> gains;

    explicit Avs3HoaRenderer(int channels)
        : input_channels(channels), gains(channels, std::vector<double>(2))
    {
        if (channels != 4 && channels != 9 && channels != 16)
            throw std::invalid_argument("Unsupported AV3A HOA order");

        ear::HOATypeMetadata metadata;
        metadata.normalization = "N3D";
        for (int channel = 0; channel < channels; channel++) {
            int order = static_cast<int>(std::sqrt(channel));
            metadata.orders.push_back(order);
            metadata.degrees.push_back(channel - order * (order + 1));
        }
        ear::GainCalculatorHOA calculator(ear::getLayout("0+2+0"));
        calculator.calculate(metadata, gains);
        for (const auto &channel : gains)
            for (double gain : channel)
                if (!std::isfinite(gain))
                    throw std::invalid_argument("Invalid AV3A HOA speaker gain");
    }

    void process(int16_t *pcm, int samples, int channels) const
    {
        if (samples <= 0 || samples > 4096 || channels != input_channels)
            throw std::invalid_argument("Invalid AV3A HOA PCM dimensions");

        for (int sample = 0; sample < samples; sample++) {
            double mixed[2] = {};
            for (int channel = 0; channel < channels; channel++)
                for (int speaker = 0; speaker < 2; speaker++)
                    mixed[speaker] += pcm[sample * channels + channel] * gains[channel][speaker];
            for (int speaker = 0; speaker < 2; speaker++)
                pcm[sample * 2 + speaker] = static_cast<int16_t>(std::lrint(
                    std::max(-32768.0, std::min(32767.0, mixed[speaker]))));
        }
    }
};

extern "C" int avs3_hoa_renderer_create(Avs3HoaRenderer **renderer, int input_channels)
{
    if (!renderer)
        return -EINVAL;
    *renderer = nullptr;
    try {
        *renderer = new Avs3HoaRenderer(input_channels);
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::exception &) {
        return -EINVAL;
    }
}

extern "C" int avs3_hoa_renderer_process(Avs3HoaRenderer *renderer, int16_t *pcm,
                                         int samples, int input_channels, char *error,
                                         size_t error_size)
{
    if (!renderer || !pcm)
        return -EINVAL;
    try {
        renderer->process(pcm, samples, input_channels);
        return 0;
    } catch (const std::exception &e) {
        if (error && error_size)
            std::snprintf(error, error_size, "%s", e.what());
        return -EINVAL;
    }
}

extern "C" void avs3_hoa_renderer_destroy(Avs3HoaRenderer *renderer)
{
    delete renderer;
}
