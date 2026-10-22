// SBC, the mandatory A2DP codec: fallback when the sink rejects LDAC.
// Encoder: AOSP system/embdrv/sbc/encoder (third_party/sbc_encoder).
#include "svc.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "avdtp_media.h"
#include "sbc_encoder.h"
}

namespace {

int Pick(int bit, std::initializer_list<std::pair<int, int>> map)
{
    for (auto &m : map)
        if (bit == m.first)
            return m.second;
    return -1;
}

class SbcEncoder : public AudioEncoder {
public:
    bool Init(const LinkInfo &link, const DeviceSettings &, std::string &err) override
    {
        const a2dp_sbc &c = link.sbc;
        memset(&params_, 0, sizeof(params_));
        params_.s16SamplingFreq = (int16_t)Pick(c.freq, { { SBC_FREQ_16000, SBC_sf16000 }, { SBC_FREQ_32000, SBC_sf32000 },
                                                          { SBC_FREQ_44100, SBC_sf44100 }, { SBC_FREQ_48000, SBC_sf48000 } });
        params_.s16ChannelMode = (int16_t)Pick(c.cm, { { SBC_CM_MONO, SBC_MONO }, { SBC_CM_DUAL, SBC_DUAL },
                                                       { SBC_CM_STEREO, SBC_STEREO }, { SBC_CM_JOINT, SBC_JOINT_STEREO } });
        params_.s16NumOfBlocks = (int16_t)Pick(c.block, { { SBC_BLOCK_4, 4 }, { SBC_BLOCK_8, 8 }, { SBC_BLOCK_12, 12 },
                                                          { SBC_BLOCK_16, 16 } });
        params_.s16NumOfSubBands = (int16_t)Pick(c.subbands, { { SBC_SUBBANDS_4, 4 }, { SBC_SUBBANDS_8, 8 } });
        params_.s16AllocationMethod = (int16_t)Pick(c.alloc, { { SBC_ALLOC_LOUDNESS, SBC_LOUDNESS }, { SBC_ALLOC_SNR, SBC_SNR } });
        if (params_.s16SamplingFreq < 0 || params_.s16ChannelMode < 0 || params_.s16NumOfBlocks < 0 ||
            params_.s16NumOfSubBands < 0 || params_.s16AllocationMethod < 0 || c.max_bitpool < 2)
            return err = "invalid SBC configuration", false;
        params_.Format = SBC_FORMAT_GENERAL;
        params_.u16BitRate = 328;  // only seeds SBC_Encoder_Init; the bitpool is set below
        SBC_Encoder_Init(&params_);
        // The highest bitpool the sink accepts, capped at the usual high-quality 53.
        bitpool_ = std::min<int>(c.max_bitpool, SBC_BITPOOL_HQ);
        params_.s16BitPool = (int16_t)bitpool_;

        channels_ = params_.s16NumOfChannels;
        samplesPerFrame_ = params_.s16NumOfBlocks * params_.s16NumOfSubBands;
        rate_ = link.sampleRate;
        mtu_ = (size_t)link.mtu;
        avdtp_media_init(&media_, 0);
        media_.ssrc = 0;

        // Frame length per the A2DP/SBC spec, to pack as many frames as fit.
        int nb = params_.s16NumOfBlocks, ns = params_.s16NumOfSubBands, ch = channels_;
        int bits = params_.s16ChannelMode == SBC_MONO || params_.s16ChannelMode == SBC_DUAL
                       ? nb * ch * bitpool_
                       : (params_.s16ChannelMode == SBC_JOINT_STEREO ? ns : 0) + nb * bitpool_;
        frameLen_ = 4 + (4 * ns * ch) / 8 + (bits + 7) / 8;
        framesPerPacket_ = std::clamp<int>((int)((mtu_ - A2DP_MEDIA_HDR_LEN) / frameLen_), 1, 15);
        return true;
    }

    void Encode(const float *frames, size_t count, std::vector<std::vector<uint8_t>> &packets) override
    {
        for (size_t i = 0; i < count * channels_; i++)
            pcm_.push_back((int16_t)std::clamp(frames[i] * 32768.0f, -32768.0f, 32767.0f));

        size_t frameSamples = (size_t)samplesPerFrame_ * channels_;
        size_t used = 0;
        while (pcm_.size() - used >= frameSamples) {
            uint8_t frame[512];
            uint32_t n = SBC_Encode(&params_, pcm_.data() + used, frame);
            used += frameSamples;
            pending_.insert(pending_.end(), frame, frame + n);
            if (++pendingFrames_ == framesPerPacket_)
                Flush(packets);
        }
        pcm_.erase(pcm_.begin(), pcm_.begin() + used);
    }

    std::string Describe() const override
    {
        return "SBC " + std::to_string(rate_) + " Hz, " + std::to_string(params_.s16NumOfBlocks) + " blocks, " +
               std::to_string(params_.s16NumOfSubBands) + " subbands, bitpool " + std::to_string(bitpool_) + ", " +
               std::to_string(framesPerPacket_) + " frames/packet";
    }

    std::string Quality() const override { return "bp" + std::to_string(bitpool_); }
    int NominalKbps() const override { return (int)((int64_t)frameLen_ * 8 * rate_ / samplesPerFrame_ / 1000); }
    int BitDepth() const override { return 16; }

private:
    void Flush(std::vector<std::vector<uint8_t>> &packets)
    {
        std::vector<uint8_t> pkt(mtu_);
        size_t len = avdtp_media_packet(&media_, pkt.data(), mtu_, pending_.data(), pending_.size(),
                                        (unsigned)pendingFrames_, (uint32_t)(pendingFrames_ * samplesPerFrame_));
        if (len) {
            pkt.resize(len);
            packets.push_back(std::move(pkt));
        }
        pending_.clear();
        pendingFrames_ = 0;
    }

    SBC_ENC_PARAMS params_{};
    avdtp_media_stream media_{};
    std::vector<int16_t> pcm_;
    std::vector<uint8_t> pending_;
    int pendingFrames_ = 0;
    int framesPerPacket_ = 1;
    int frameLen_ = 0;
    int bitpool_ = SBC_BITPOOL_HQ;
    int channels_ = 2;
    int samplesPerFrame_ = 128;
    uint32_t rate_ = 48000;
    size_t mtu_ = 679;
};

}  // namespace

std::unique_ptr<AudioEncoder> MakeSbcEncoder()
{
    return std::make_unique<SbcEncoder>();
}
