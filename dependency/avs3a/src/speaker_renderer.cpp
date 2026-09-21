/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "speaker_renderer.h"
#include "avs3_stat_com.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

#include <ear/ear.hpp>
#include <ear/conversion.hpp>

namespace
{

constexpr int max_channels = 32;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::invalid_argument(message);
}

ear::Layout get_layout(int config)
{
    switch (config) {
    case CHANNEL_CONFIG_STEREO:
        return ear::getLayout("0+2+0");
    case CHANNEL_CONFIG_MC_5_1:
        return ear::getLayout("0+5+0");
    case CHANNEL_CONFIG_MC_7_1_4:
        return ear::getLayout("4+7+0");
    default:
        throw ear::not_implemented("AV3A sound-bed speaker layout");
    }
}

float linear_gain(float value, int unit)
{
    require(unit == 0 || unit == 1, "Invalid AV3A gain unit");
    float result = unit == 0 ? value : std::pow(10.0f, value / 20.0f);
    require(std::isfinite(result) && result >= 0, "Invalid AV3A gain");
    return result;
}

template <class T, size_t N, class Predicate>
const T &find_entry(const T (&entries)[N], int count, Predicate matches)
{
    require(count >= 0 && count <= static_cast<int>(N), "Invalid AV3A metadata count");
    for (int i = 0; i < count; i++)
        if (matches(entries[i]))
            return entries[i];
    throw std::invalid_argument("Unresolved AV3A metadata reference");
}

} // namespace

struct Avs3SpeakerRenderer {
    ear::Layout layout;
    ear::GainCalculatorObjects calculator;
    int channels;
    int content_index;
    bool have_static_metadata = false;
    bool have_previous_gains = false;
    Avs3MetaDataStatic static_metadata = {};
    float previous[max_channels][max_channels] = {};
    std::vector<float> direct;
    std::vector<float> diffuse;

    Avs3SpeakerRenderer(int config, int output_channels, int content)
        : layout(get_layout(config)), calculator(layout), channels(output_channels),
          content_index(content), direct(channels), diffuse(channels)
    {
        require(channels == static_cast<int>(layout.channels().size()),
                "AV3A sound-bed size mismatch");
        require(content_index >= 0 && content_index < 4, "Invalid AV3A content index");
    }

    void object_gains(const Avs3MetaData &metadata, int input_channel, float gain, int gain_unit,
                      float *gains, bool &interpolate)
    {
        const auto &dynamic = metadata.avs3MetaDataDynamic;
        require(dynamic.numDmChans > 0 && dynamic.numDmChans <= max_channels,
                "Missing AV3A object metadata");
        int index = -1;
        for (int i = 0; i < dynamic.numDmChans; i++) {
            if (dynamic.transChRef[i] == input_channel) {
                require(index == -1, "Duplicate AV3A object metadata");
                index = i;
            }
        }
        require(index >= 0, "Missing AV3A object position");
        if (dynamic.muteFlag[index])
            return;

        const auto &dm = dynamic.avs3DmL1MetaData[index];
        interpolate = !dm.jumpPosition;
        ear::ObjectsTypeMetadata object;
        if (dm.cartesian) {
            require(std::isfinite(dm.obj_x) && std::isfinite(dm.obj_y) && std::isfinite(dm.obj_z),
                    "Invalid AV3A Cartesian position");
            object.cartesian = true;
            object.position = ear::CartesianPosition(dm.obj_x, dm.obj_y, dm.obj_z);
            if (dm.hasObjExtent) {
                object.width = dm.objWidth_x;
                object.height = dm.objHeight_y;
                object.depth = dm.objDepth_z;
            }
        } else {
            require(std::isfinite(dm.objAzimuth) && std::isfinite(dm.objElevation) &&
                        std::isfinite(dm.objDistance) && std::abs(dm.objAzimuth) <= 180 &&
                        std::abs(dm.objElevation) <= 90 && dm.objDistance >= 0,
                    "Invalid AV3A polar position");
            object.position = ear::PolarPosition(dm.objAzimuth, dm.objElevation, dm.objDistance);
            if (dm.hasObjExtent) {
                object.width = dm.objWidth;
                object.height = dm.objHeight;
                object.depth = dm.objDepth;
            }
        }
        require(std::isfinite(object.width) && std::isfinite(object.height) &&
                    std::isfinite(object.depth) && object.width >= 0 && object.height >= 0 &&
                    object.depth >= 0,
                "Invalid AV3A object extent");
        if (dm.cartesian)
            ear::conversion::toPolar(object);
        object.gain = dm.hasObjGain ? linear_gain(dm.gain, gain_unit) : gain;
        if (dm.hasObjDiffuse && dm.diffuse != 0)
            throw ear::not_implemented("AV3A diffuse object rendering");
        require(dynamic.dmLevel == 0 || dynamic.dmLevel == 1, "Invalid AV3A metadata level");
        if (dynamic.dmLevel == 1) {
            const auto &l2 = dynamic.avs3DmL2MetaData[index];
            if ((l2.hasChannelLock && l2.channelLock) ||
                (l2.hasObjectDivergence && l2.objDivergence != 0) ||
                (l2.hasObjectScreenRef && l2.objScreenRef) || l2.hasScreenEdgeLock)
                throw ear::not_implemented("AV3A extended object rendering");
        }
        calculator.calculate(object, direct, diffuse);
        for (int c = 0; c < channels; c++) {
            require(std::isfinite(direct[c]), "Invalid AV3A speaker gain");
            gains[c] = direct[c];
        }
    }

    void process(const Avs3MetaData &metadata, int16_t *pcm, int samples, int input_channels)
    {
        require(samples > 0 && samples <= 4096 && input_channels > channels &&
                    input_channels <= max_channels,
                "Invalid AV3A PCM dimensions");
        if (metadata.hasStaticMeta) {
            static_metadata = metadata.avs3MetaDataStatic;
            have_static_metadata = true;
        }
        require(have_static_metadata, "Missing AV3A static metadata");
        if (static_metadata.hasVrExt)
            throw ear::not_implemented("AV3A VR environment rendering");
        const auto &basic = static_metadata.avs3BasicL1;
        const auto &programme = basic.audioProgrammeMeta;
        require(programme.numContents > content_index && programme.numContents <= 4,
                "AV3A content index is unavailable");
        const auto &content =
            find_entry(basic.audioContentData, basic.numOfContents, [&](const AudioContent &entry) {
                return entry.contentIdx == programme.refContentIdx[content_index];
            });
        require(content.numObjects > 0 && content.numObjects <= 8,
                "Invalid AV3A content object count");
        if (content.numComplementaryObjectGroup)
            throw ear::not_implemented("AV3A complementary object selection");
        float matrix[max_channels][max_channels] = {};
        bool assigned[max_channels] = {};
        bool interpolate[max_channels] = {};
        for (int oi = 0; oi < content.numObjects; oi++) {
            const auto &object = find_entry(basic.audioObjectData, basic.numOfObjects,
                                            [&](const AudioObject &entry) {
                                                return entry.objectIdx == content.refObjectIdx[oi];
                                            });
            if (object.hasHeadLocked)
                throw ear::not_implemented("AV3A head-locked object rendering");
            require(object.numPacks > 0 && object.numPacks <= 8, "Invalid AV3A pack count");
            for (int pi = 0; pi < object.numPacks; pi++) {
                const auto &pack = find_entry(
                    basic.audioPackFormatData, basic.numOfPacks, [&](const AudioPackFormat &entry) {
                        return entry.packFormatIdx == object.refPackFormatIdx[pi];
                    });
                require(pack.numChannels > 0 && pack.numChannels <= max_channels,
                        "Invalid AV3A pack channel count");
                if (pack.typeLabel != 1 && pack.typeLabel != 3)
                    throw ear::not_implemented("AV3A matrix or HOA content rendering");
                if (pack.typeLabel == 1 && pack.packFormatID == 0x3f)
                    throw ear::not_implemented("AV3A custom sound-bed positions");
                for (int ci = 0; ci < pack.numChannels; ci++) {
                    int input =
                        pack.hasChannelReuse ? pack.transChRef[ci] : pack.packFormatStartIdx + ci;
                    require(input >= 0 && input < input_channels,
                            "Invalid AV3A PCM channel reference");
                    require(!assigned[input], "Duplicate AV3A PCM channel reference");
                    assigned[input] = true;
                    const auto &channel =
                        find_entry(basic.audioChannelFormatData, basic.numOfChannels,
                                   [&](const AudioChannelFormat &entry) {
                                       return entry.channelFormatIdx == pack.refChannelIdx[ci];
                                   });
                    float gain = 1;
                    int gain_unit = 0;
                    if (channel.hasChannelGain) {
                        gain_unit = channel.gainUnit;
                        gain = linear_gain(channel.channelGain, gain_unit);
                    } else if (object.hasGain) {
                        gain_unit = object.gainUnit;
                        gain = linear_gain(object.gain, gain_unit);
                    }
                    if (object.hasMute)
                        continue;
                    if (pack.typeLabel == 1) {
                        require(input < channels, "AV3A sound-bed channel is outside the bed");
                        matrix[input][input] = gain;
                    } else {
                        require(input >= channels, "AV3A object channel overlaps the bed");
                        object_gains(metadata, input, gain, gain_unit, matrix[input],
                                     interpolate[input]);
                    }
                }
            }
        }
        // Reduce the interleaved buffer in place. Read all input channels for a
        // sample before writing, including when input and output start together.
        double mixed[max_channels];
        for (int s = 0; s < samples; s++) {
            for (int c = 0; c < channels; c++) {
                double value = 0;
                for (int in = 0; in < input_channels; in++) {
                    float gain = matrix[in][c];
                    if (have_previous_gains && interpolate[in])
                        gain = previous[in][c] + (gain - previous[in][c]) * (s + 1) / samples;
                    value += pcm[s * input_channels + in] * static_cast<double>(gain);
                }
                mixed[c] = std::max(-32768.0, std::min(32767.0, value));
            }
            for (int c = 0; c < channels; c++)
                pcm[s * channels + c] = std::lrint(mixed[c]);
        }
        std::memcpy(previous, matrix, sizeof(previous));
        have_previous_gains = true;
    }
};

extern "C" int avs3_speaker_renderer_create(Avs3SpeakerRenderer **renderer, int channel_config,
                                            int channels, int content_index)
{
    if (!renderer || channels <= 0 || channels > max_channels)
        return -EINVAL;
    *renderer = nullptr;
    try {
        *renderer = new Avs3SpeakerRenderer(channel_config, channels, content_index);
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const ear::not_implemented &) {
        return -ENOSYS;
    } catch (const std::exception &) {
        return -EINVAL;
    }
}

extern "C" int avs3_speaker_renderer_process(Avs3SpeakerRenderer *renderer,
                                             const Avs3MetaData *metadata, int16_t *pcm,
                                             int samples, int input_channels, char *error,
                                             size_t error_size)
{
    if (!renderer || !metadata || !pcm)
        return -EINVAL;
    try {
        renderer->process(*metadata, pcm, samples, input_channels);
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const ear::not_implemented &e) {
        if (error && error_size)
            std::snprintf(error, error_size, "%s", e.what());
        return -ENOSYS;
    } catch (const std::exception &e) {
        if (error && error_size)
            std::snprintf(error, error_size, "%s", e.what());
        return -EINVAL;
    }
}

extern "C" void avs3_speaker_renderer_reset(Avs3SpeakerRenderer *renderer)
{
    if (renderer)
        renderer->have_previous_gains = false;
}

extern "C" void avs3_speaker_renderer_destroy(Avs3SpeakerRenderer *renderer)
{
    delete renderer;
}
