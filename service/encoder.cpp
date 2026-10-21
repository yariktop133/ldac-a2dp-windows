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

#define H(p) static_cast<HANDLE_LDAC_BT>(p)
#define A(p) static_cast<HANDLE_LDAC_ABR>(p)
#define M(p) static_cast<avdtp_media_stream *>(p)

LdacEncoder::~LdacEncoder()
{
    if (abr_)
        ldac_ABR_free_handle(A(abr_));
    if (ldac_)
        ldacBT_free_handle(H(ldac_));
    delete M(media_);
}

bool LdacEncoder::Init(const LinkInfo &link, int eqmid, bool adaptive, bool s16, std::string &err)
{
    if (link.codec != "LDAC")
        return err = "codec " + link.codec + " is not supported by the encoder yet", false;
    ldac_ = ldacBT_get_handle();
    if (!ldac_)
        return err = "ldacBT_get_handle failed", false;
    s16_ = s16;
    channels_ = link.channels;
    bytesPerFrame_ = channels_ * (s16 ? 2 : 4);
    mtu_ = (size_t)link.mtu;
    rate_ = link.sampleRate;
    maxEqmidKbps_ = EqmidKbps(eqmid, rate_);
    eqmid_ = eqmid;
    if (ldacBT_init_handle_encode(H(ldac_), link.mtu, eqmid, link.ldacCm,
                                  s16 ? LDACBT_SMPL_FMT_S16 : LDACBT_SMPL_FMT_F32, (int)link.sampleRate) != 0)
        return err = "ldacBT_init_handle_encode failed, error " + std::to_string(ldacBT_get_error_code(H(ldac_))) +
                     " (rate " + std::to_string(link.sampleRate) + ", MTU " + std::to_string(link.mtu) + ")", false;
    if (adaptive) {
        abr_ = ldac_ABR_get_handle();
        if (!abr_ || ldac_ABR_Init(A(abr_), kAbrIntervalMs) != 0)
            return err = "ldac_ABR_Init failed", false;
    }
    media_ = new avdtp_media_stream;
    avdtp_media_init(M(media_), 0);
    M(media_)->ssrc = 0; /* as in the captured reference stream */
    return true;
}

void LdacEncoder::Encode(const float *frames, size_t count, std::vector<std::vector<uint8_t>> &packets)
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
        if (ldacBT_encode(H(ldac_), acc_.data() + offset, &used, stream, &size, &nframes) != 0 || used <= 0)
            break;
        offset += (size_t)used;
        if (size > 0) {
            std::vector<uint8_t> pkt(mtu_);
            // RTP timestamp: 128 per LDAC frame at every rate (docs/jbl-live-780nc.md)
            size_t len = avdtp_media_packet(M(media_), pkt.data(), mtu_, stream, (size_t)size, (unsigned)nframes,
                                            (uint32_t)(nframes * LDACBT_ENC_LSU));
            if (len) {
                pkt.resize(len);
                packets.push_back(std::move(pkt));
            }
        }
    }
    acc_.erase(acc_.begin(), acc_.begin() + offset);
}

void LdacEncoder::Tick(unsigned txQueueDepth)
{
    if (!abr_)
        return;
    ldac_ABR_Proc(H(ldac_), A(abr_), txQueueDepth, 1);
    // ABR only lowers the quality the user chose: never climb above it.
    if (EqmidKbps(Eqmid(), rate_) > maxEqmidKbps_)
        ldacBT_set_eqmid(H(ldac_), eqmid_);
}

int LdacEncoder::Eqmid() const
{
    return ldac_ ? ldacBT_get_eqmid(H(ldac_)) : -1;
}

int LdacEncoder::NominalKbps() const
{
    return EqmidKbps(Eqmid(), rate_);
}
