// ldacsvc: user-mode encoder service for the LdacA2dp driver.
//
//   audio source (WASAPI loopback / WAV) -> resampler -> libldac (+ABR)
//   -> A2DP media packets -> sink (LdacA2dp driver, or a simulated link)
//
// Settings come from %ProgramData%\LdacA2dp\settings.json (written by
// ui/LdacConfig), live state goes to %ProgramData%\LdacA2dp\status\<addr>.json
// (shown by the same UI). See docs/settings-format.md.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "a2dp_codec.h"
}

// ---- settings / status (settings.cpp) -------------------------------------

struct DeviceSettings {
    bool ldac = true;
    uint8_t freqMask = LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000;
    uint8_t cmMask = LDAC_CM_STEREO;
    bool dualChannel = false;  // "Стерео-режим" = Dual Channel
    bool bits24 = true;
    int eqmid = 1;             // LDACBT_EQMID_SQ
    bool adaptive = true;
};

std::string DataRoot();  // %ProgramData%\LdacA2dp
// Missing file or device: defaults, returns false with a reason in `note`.
bool LoadSettings(const std::string &address, DeviceSettings &out, std::string &note);
a2dp_prefs ToPrefs(const DeviceSettings &s);

struct LinkInfo {
    std::string codec;      // "LDAC" (SBC is not encoded yet)
    uint32_t sampleRate = 0;
    int channels = 2;
    uint8_t ldacCm = LDAC_CM_STEREO;
    int mtu = 679;
    uint16_t delay100us = 0;
};

void WriteStatus(const std::string &address, const LinkInfo &link, int bitDepth, int kbps);
void ClearStatus(const std::string &address);
std::string ChannelModeName(uint8_t cm);

// ---- audio sources (capture.cpp) ------------------------------------------

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual bool Start(std::string &err) = 0;
    virtual uint32_t Rate() const = 0;
    virtual int Channels() const = 0;
    virtual std::string Describe() const = 0;
    // Appends interleaved float frames that became available, waiting up to
    // timeoutMs. Returns false at the end of a finite source.
    virtual bool Read(std::vector<float> &out, int timeoutMs) = 0;
};

std::unique_ptr<AudioSource> MakeLoopbackSource(const std::string &endpointMatch);
std::unique_ptr<AudioSource> MakeWavSource(const std::string &path);

// ---- resampler (resampler.cpp) -----------------------------------------------

class Resampler {
public:
    Resampler(double inRate, double outRate, int channels);
    void Process(const float *in, size_t frames, std::vector<float> &out);

private:
    static constexpr int kHalfTaps = 16;
    double step_, cutoff_, pos_;
    int ch_;
    bool passthrough_;
    std::vector<float> hist_;
    std::vector<double> kernel_;
};

// ---- LDAC encoder (encoder.cpp) ----------------------------------------------

class LdacEncoder {
public:
    ~LdacEncoder();
    bool Init(const LinkInfo &link, int eqmid, bool adaptive, bool s16, std::string &err);
    // Encodes interleaved float frames (link.channels wide); every finished
    // media packet (RTP + payload header + frames) is appended to `packets`.
    void Encode(const float *frames, size_t count, std::vector<std::vector<uint8_t>> &packets);
    void Tick(unsigned txQueueDepth);  // every kAbrIntervalMs
    int Eqmid() const;
    int NominalKbps() const;

    static constexpr unsigned kAbrIntervalMs = 20;

private:
    void *ldac_ = nullptr;  // HANDLE_LDAC_BT
    void *abr_ = nullptr;   // HANDLE_LDAC_ABR
    std::vector<uint8_t> acc_;
    int bytesPerFrame_ = 0;
    bool s16_ = false;
    int channels_ = 2;
    size_t mtu_ = 679;
    uint32_t rate_ = 48000;
    int eqmid_ = 1;         // quality the user chose: ABR never goes above it
    int maxEqmidKbps_ = 660;
    void *media_ = nullptr; // avdtp_media_stream
};

const char *EqmidName(int eqmid);
int EqmidKbps(int eqmid, uint32_t rate);

// ---- sinks (sinks.cpp) -------------------------------------------------------

class MediaSink {
public:
    virtual ~MediaSink() = default;
    virtual bool Open(const a2dp_prefs &prefs, LinkInfo &link, std::string &err) = 0;
    virtual bool Start(std::string &err) = 0;
    virtual void Suspend() = 0;
    virtual bool Send(const uint8_t *packet, size_t len) = 0;  // false: dropped
    virtual void Pump() = 0;                                   // collect completions
    virtual unsigned QueueDepth() const = 0;
    virtual uint16_t Delay100us() { return 0; }
    virtual std::string Describe() const = 0;
};

// address 0: the first LdacA2dp device found
std::unique_ptr<MediaSink> MakeDriverSink(uint64_t address);
// Simulated radio link: capacity in kbit/s over time ("0:1500,5:400").
std::unique_ptr<MediaSink> MakeNullSink(const std::string &linkSpec);

uint64_t ParseAddress(const std::string &text);
std::string FormatAddress(uint64_t address);
