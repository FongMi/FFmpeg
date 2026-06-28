/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "speaker_renderer.h"
#include "avs3_stat_com.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

static Avs3MetaData metadata(int bed, int objects)
{
    Avs3MetaData m = {};
    m.hasStaticMeta = m.hasDynamicMeta = 1;
    auto &b = m.avs3MetaDataStatic.avs3BasicL1;
    b.audioProgrammeMeta.numContents = objects;
    b.numOfContents = objects;
    b.numOfObjects = b.numOfPacks = objects + 1;
    b.numOfChannels = bed + objects;
    for (int c = 0; c < objects; c++) {
        b.audioProgrammeMeta.refContentIdx[c] = c;
        b.audioContentData[c].contentIdx = c;
        b.audioContentData[c].numObjects = 2;
        b.audioContentData[c].refObjectIdx[1] = c + 1;
    }
    for (int i = 0; i <= objects; i++) {
        auto &o = b.audioObjectData[i];
        auto &p = b.audioPackFormatData[i];
        o.objectIdx = p.packFormatIdx = i;
        o.numPacks = 1;
        o.refPackFormatIdx[0] = i;
        p.typeLabel = i ? 3 : 1;
        p.packFormatStartIdx = i ? bed + i - 1 : 0;
        p.numChannels = i ? 1 : bed;
        for (int c = 0; c < p.numChannels; c++)
            p.refChannelIdx[c] = p.packFormatStartIdx + c;
    }
    for (int c = 0; c < bed + objects; c++)
        b.audioChannelFormatData[c].channelFormatIdx = c;
    auto &d = m.avs3MetaDataDynamic;
    d.numDmChans = objects;
    for (int i = 0; i < objects; i++) {
        d.transChRef[i] = bed + i;
        d.avs3DmL1MetaData[i].objDistance = 1;
    }
    return m;
}

static Avs3SpeakerRenderer *create(int config, int bed, int content = 0)
{
    Avs3SpeakerRenderer *r = nullptr;
    assert(avs3_speaker_renderer_create(&r, config, bed, content) == 0);
    return r;
}

static int process(Avs3SpeakerRenderer *r, Avs3MetaData &m, std::vector<int16_t> &pcm, int channels,
                   int samples = 1)
{
    char error[256] = {};
    return avs3_speaker_renderer_process(r, &m, pcm.data(), samples, channels, error,
                                         sizeof(error));
}

int main()
{
    // Directional impulses must reach their corresponding speaker, including heights.
    const float angles[][4] = {{30, 0, 0, 6},   {-30, 0, 1, 6},  {0, 0, 2, 6},      {110, 0, 4, 6},
                               {-110, 0, 5, 6}, {45, 30, 8, 12}, {-135, 30, 11, 12}};
    for (const auto &angle : angles) {
        int bed = angle[3], expected = angle[2];
        auto m = metadata(bed, 1);
        auto &dm = m.avs3MetaDataDynamic.avs3DmL1MetaData[0];
        dm.objAzimuth = angle[0];
        dm.objElevation = angle[1];
        auto r = create(bed == 6 ? CHANNEL_CONFIG_MC_5_1 : CHANNEL_CONFIG_MC_7_1_4, bed);
        std::vector<int16_t> pcm(bed + 1);
        pcm[bed] = 1000;
        assert(process(r, m, pcm, bed + 1) == 0);
        for (int c = 0; c < bed; c++)
            assert(std::abs(pcm[c] - (c == expected ? 1000 : 0)) <= 1);
        avs3_speaker_renderer_destroy(r);
    }
    // Selecting one presentation preserves its bed and its object, without mixing alternatives.
    for (int selected = 0; selected < 3; selected++) {
        auto m = metadata(6, 3);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6, selected);
        std::vector<int16_t> pcm = {10, 20, 30, 40, 50, 60, 100, 200, 300,
                                    11, 21, 31, 41, 51, 61, 101, 201, 301};
        assert(process(r, m, pcm, 9, 2) == 0);
        for (int s = 0; s < 2; s++)
            for (int c = 0; c < 6; c++)
                assert(pcm[s * 6 + c] ==
                       10 * (c + 1) + s + (c == 2 ? 100 * (selected + 1) + s : 0));
        avs3_speaker_renderer_destroy(r);
    }
    // Stereo keeps distinct bed channels and adds a centered object at equal-power gains.
    {
        auto m = metadata(2, 1);
        auto r = create(CHANNEL_CONFIG_STEREO, 2);
        std::vector<int16_t> pcm = {100, 200, 1000};
        assert(process(r, m, pcm, 3) == 0);
        assert(pcm[0] == 807 && pcm[1] == 907);
        avs3_speaker_renderer_destroy(r);
    }
    // Quantization clips to S16 only after the complete mix.
    {
        auto m = metadata(2, 1);
        auto r = create(CHANNEL_CONFIG_STEREO, 2);
        // Keep the product and accumulation in double precision until S16 rounding.
        std::vector<int16_t> pcm = {20000, 20000, -23949};
        assert(process(r, m, pcm, 3) == 0);
        assert(pcm[0] == 3065 && pcm[1] == 3065);
        pcm = {30000, -30000, 10000};
        assert(process(r, m, pcm, 3) == 0 && pcm[0] == 32767 && pcm[1] == -22929);
        pcm = {30000, -30000, -10000};
        assert(process(r, m, pcm, 3) == 0 && pcm[0] == 22929 && pcm[1] == -32768);
        avs3_speaker_renderer_destroy(r);
    }
    // Static gain, dynamic mute and metadata retained between frames.
    {
        auto m = metadata(6, 1);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6);
        m.avs3MetaDataStatic.avs3BasicL1.audioObjectData[1].hasGain = 1;
        m.avs3MetaDataStatic.avs3BasicL1.audioObjectData[1].gain = 0.5f;
        std::vector<int16_t> pcm(7);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0 && pcm[2] == 500);
        m.hasStaticMeta = 0;
        pcm.assign(7, 0);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0 && pcm[2] == 500);
        m.avs3MetaDataDynamic.muteFlag[0] = 1;
        pcm.assign(7, 0);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0 && pcm[2] == 0);
        avs3_speaker_renderer_destroy(r);
        r = create(CHANNEL_CONFIG_MC_5_1, 6);
        assert(process(r, m, pcm, 7) == -EINVAL);
        avs3_speaker_renderer_destroy(r);
    }
    // Channel references, Cartesian positions and static/dynamic gain precedence.
    {
        auto m = metadata(6, 1);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6);
        auto &basic = m.avs3MetaDataStatic.avs3BasicL1;
        basic.audioObjectData[1].hasGain = 1;
        basic.audioObjectData[1].gain = 0.1f;
        auto &channel = basic.audioChannelFormatData[6];
        channel.channelFormatIdx = 23;
        channel.hasChannelGain = 1;
        channel.gainUnit = 1;
        channel.channelGain = -6.0206f;
        auto &pack = basic.audioPackFormatData[1];
        pack.refChannelIdx[0] = 23;
        pack.hasChannelReuse = 1;
        pack.transChRef[0] = 6;
        pack.packFormatStartIdx = 0;
        auto &dm = m.avs3MetaDataDynamic.avs3DmL1MetaData[0];
        dm.cartesian = 1;
        dm.obj_y = 1;
        std::vector<int16_t> pcm(7);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0 && pcm[2] == 500);
        dm.hasObjGain = 1;
        dm.gain = 0;
        pcm.assign(7, 0);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0 && pcm[2] == 1000);
        dm.hasObjExtent = 1;
        dm.objWidth_x = std::numeric_limits<float>::quiet_NaN();
        auto original = pcm;
        assert(process(r, m, pcm, 7) == -EINVAL && pcm == original);
        avs3_speaker_renderer_destroy(r);
    }
    // Custom bed positions cannot be treated as a standard channel order.
    {
        auto m = metadata(6, 1);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6);
        m.avs3MetaDataStatic.avs3BasicL1.audioPackFormatData[0].packFormatID = 0x3f;
        std::vector<int16_t> pcm = {1, 2, 3, 4, 5, 6, 1000};
        auto original = pcm;
        assert(process(r, m, pcm, 7) == -ENOSYS && pcm == original);
        avs3_speaker_renderer_destroy(r);
    }
    // Changes without jumpPosition interpolate; a fresh renderer has no previous stream's gains.
    {
        auto m = metadata(6, 1);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6);
        std::vector<int16_t> pcm(7);
        pcm[6] = 1000;
        assert(process(r, m, pcm, 7) == 0);
        m.avs3MetaDataDynamic.avs3DmL1MetaData[0].objAzimuth = 30;
        pcm.assign(14, 0);
        pcm[6] = pcm[13] = 1000;
        assert(process(r, m, pcm, 7, 2) == 0);
        assert(pcm[0] == 500 && pcm[2] == 500 && pcm[6] == 1000 && pcm[8] == 0);
        // Seeking recreates the decoder, whose static metadata starts empty.
        avs3_speaker_renderer_reset(r);
        m.hasStaticMeta = 0;
        m.avs3MetaDataStatic = {};
        m.avs3MetaDataDynamic.avs3DmL1MetaData[0].objAzimuth = -30;
        pcm.assign(14, 0);
        pcm[6] = pcm[13] = 1000;
        assert(process(r, m, pcm, 7, 2) == 0);
        assert(pcm[0] == 0 && pcm[1] == 1000 && pcm[7] == 1000);
        avs3_speaker_renderer_destroy(r);
    }
    // Invalid references and unsupported rendering cannot silently produce bed-only output.
    {
        auto m = metadata(6, 1);
        auto r = create(CHANNEL_CONFIG_MC_5_1, 6);
        std::vector<int16_t> pcm(7);
        pcm[6] = 1000;
        m.avs3MetaDataDynamic.transChRef[0] = 31;
        assert(process(r, m, pcm, 7) == -EINVAL && pcm[6] == 1000);
        m = metadata(6, 1);
        m.avs3MetaDataDynamic.avs3DmL1MetaData[0].hasObjDiffuse = 1;
        m.avs3MetaDataDynamic.avs3DmL1MetaData[0].diffuse = 0.5f;
        assert(process(r, m, pcm, 7) == -ENOSYS && pcm[6] == 1000);
        m = metadata(6, 1);
        m.avs3MetaDataDynamic.avs3DmL1MetaData[0].objAzimuth =
            std::numeric_limits<float>::quiet_NaN();
        assert(process(r, m, pcm, 7) == -EINVAL);
        avs3_speaker_renderer_destroy(r);
    }
    puts("PASS: speaker directions, heights, presentation selection, bed preservation, stereo, "
         "gain, mute, interpolation, reset and invalid metadata");
}
