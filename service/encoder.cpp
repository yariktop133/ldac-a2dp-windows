#include "svc.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "avdtp_media.h"
#include "ldacBT.h"
#include "ldacBT_abr.h"
}

// EQMID values: HQ/SQ/MQ are public, ABR also uses Q0 (492) and Q1 (396)
// from libldac's ldacBT_ex.h. Rates are for 48/96 kHz; 44.1/88.2 scale by 44.1/48.
static const char *const kEqmidNames[] = { "HQ", "SQ", "MQ", "Q0", "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7", "Q8" };
static const int kEqmidKbps[] = { 990, 660, 330, 492, 396, 282, 246, 216, 198, 180, 162, 150 };
static const int kEqmidCount = sizeof(kEqmidKbps) / sizeof(kEqmidKbps[0]);

const char *EqmidName(int eqmid)
{
    return eqmid >= 0 && eqmid < kEqmidCount ? kEqmidNames[eqmid] : "?";
}

int EqmidKbps(int eqmid, uint32_t rate)
{
    if (eqmid < 0 || eqmid >= kEqmidCount)
        return 0;
    return rate % 44100 == 0 ? kEqmidKbps[eqmid] * 441 / 480 : kEqmidKbps[eqmid];
}

namespace {

class LdacEncoder : public AudioEncoder {
public:
    ~LdacEncoder() override
    {
        if (abr_)
            ldac_ABR_free_handle(abr_);
        if (ldac_)
            ldacBT_free_handle(ldac_);
    }

    bool Init(const LinkInfo &link, const DeviceSettings &settings, std::string &err) override
    {
        ldac_ = ldacBT_get_handle();
        if (!ldac_)
            return err = "ldacBT_get_handle failed", false;
        s16_ = !settings.bits24;
        channels_ = link.channels;
        bytesPerFrame_ = channels_ * (s16_ ? 2 : 4);
        mtu_ = (size_t)link.mtu;
        rate_ = link.sampleRate;
        eqmid_ = settings.eqmid;
        maxKbps_ = EqmidKbps(eqmid_, rate_);
        adaptive_ = settings.adaptive;
        if (ldacBT_init_handle_encode(ldac_, link.mtu, eqmid_, link.ldacCm,
                                      s16_ ? LDACBT_SMPL_FMT_S16 : LDACBT_SMPL_FMT_F32, (int)link.sampleRate) != 0)
            return err = "ldacBT_init_handle_encode failed, error " + std::to_string(ldacBT_get_error_code(ldac_)) +
                         " (rate " + std::to_string(link.sampleRate) + ", MTU " + std::to_string(link.mtu) + ")", false;
        if (adaptive_) {
            abr_ = ldac_ABR_get_handle();
            if (!abr_ || ldac_ABR_Init(abr_, kAbrIntervalMs) != 0)
                return err = "ldac_ABR_Init failed", false;
        }
        avdtp_media_init(&media_, 0);
        media_.ssrc = 0; /* as in the captured reference stream */
        return true;
    }

    void Encode(const float *frames, size_t count, std::vector<std::vector<uint8_t>> &packets) override
    {
        size_t base = acc_.size();
        acc_.resize(base + count * bytesPerFrame_);
        if (s16_) {
            int16_t *dst = reinterpret_cast<int16_t *>(acc_.data() + base);
            for (size_t i = 0; i < count * channels_; i++)
                dst[i] = (int16_t)std::clamp(frames[i] * 32768.0f, -32768.0f, 32767.0f);
        } else {
            float *dst = reinterpret_cast<float *>(acc_.data() + base);
            for (size_t i = 0; i < count * channels_; i++)
                dst[i] = std::clamp(frames[i], -1.0f, 1.0f);
        }

        size_t offset = 0;
        while (acc_.size() - offset >= (size_t)LDACBT_ENC_LSU * bytesPerFrame_) {
            unsigned char stream[LDACBT_MAX_NBYTES];
            int used = 0, size = 0, nframes = 0;
            if (ldacBT_encode(ldac_, acc_.data() + offset, &used, stream, &size, &nframes) != 0 || used <= 0)
                break;
            offset += (size_t)used;
            if (size > 0) {
                std::vector<uint8_t> pkt(mtu_);
                // RTP timestamp: 128 per LDAC frame at every rate (docs/jbl-live-780nc.md)
                size_t len = avdtp_media_packet(&media_, pkt.data(), mtu_, stream, (size_t)size, (unsigned)nframes,
                                                (uint32_t)(nframes * LDACBT_ENC_LSU));
                if (len) {
                    pkt.resize(len);
                    packets.push_back(std::move(pkt));
                }
            }
        }
        acc_.erase(acc_.begin(), acc_.begin() + offset);
    }

    void Tick(unsigned txQueueDepth) override
    {
        if (!abr_)
            return;
        ldac_ABR_Proc(ldac_, abr_, txQueueDepth, 1);
        // ABR only lowers the quality the user chose: never climb above it.
        if (EqmidKbps(ldacBT_get_eqmid(ldac_), rate_) > maxKbps_)
            ldacBT_set_eqmid(ldac_, eqmid_);
    }

    std::string Describe() const override
    {
        return std::string("LDAC ") + EqmidName(eqmid_) + (adaptive_ ? " + ABR" : "") + ", " +
               (s16_ ? "16-bit" : "float") + " input";
    }

    std::string Quality() const override { return EqmidName(ldacBT_get_eqmid(ldac_)); }
    int NominalKbps() const override { return EqmidKbps(ldacBT_get_eqmid(ldac_), rate_); }
    int BitDepth() const override { return s16_ ? 16 : 24; }

private:
    HANDLE_LDAC_BT ldac_ = nullptr;
    HANDLE_LDAC_ABR abr_ = nullptr;
    avdtp_media_stream media_{};
    std::vector<uint8_t> acc_;
    int bytesPerFrame_ = 0;
    bool s16_ = false;
    bool adaptive_ = false;
    int channels_ = 2;
    size_t mtu_ = 679;
    uint32_t rate_ = 48000;
    int eqmid_ = 1;    // quality the user chose: ABR never goes above it
    int maxKbps_ = 660;
};

}  // namespace

std::unique_ptr<AudioEncoder> MakeLdacEncoder()
{
    return std::make_unique<LdacEncoder>();
}
