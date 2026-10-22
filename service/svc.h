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
bool ParseSettings(std::string json, const std::string &address, DeviceSettings &out, std::string &note);
a2dp_prefs ToPrefs(const DeviceSettings &s);

struct LinkInfo {
    std::string codec;      // "LDAC" or "SBC"
    uint32_t sampleRate = 0;
    int channels = 2;
    uint8_t ldacCm = LDAC_CM_STEREO;
    int mtu = 679;
    uint16_t delay100us = 0;
    a2dp_sbc sbc{};         // single-choice SBC configuration when codec == "SBC"
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
    // Called after the stream (re)starts, which can block for a moment: forget
    // the backlog that built up meanwhile instead of bursting it into the link.
    virtual void Resync() {}
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

// ---- encoders (encoder.cpp: LDAC, sbc.cpp: SBC) --------------------------------

class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual bool Init(const LinkInfo &link, const DeviceSettings &settings, std::string &err) = 0;
    // Encodes interleaved float frames (link.channels wide); every finished
    // media packet (RTP + payload header + frames) is appended to `packets`.
    virtual void Encode(const float *frames, size_t count, std::vector<std::vector<uint8_t>> &packets) = 0;
    // Every kAbrIntervalMs. Not suspended right after START: the link takes ~1 s
    // to ramp up, and holding the bit rate through it overflowed the queue on
    // hardware (dropped packets), while ABR stepping down drops nothing.
    virtual void Tick(unsigned txQueueDepth) { (void)txQueueDepth; }
    virtual std::string Describe() const = 0;  // configuration, for the log
    virtual std::string Quality() const = 0;   // current quality step, for the log
    virtual int NominalKbps() const = 0;
    virtual int BitDepth() const = 0;          // input resolution reported to the UI

    static constexpr unsigned kAbrIntervalMs = 20;
};

std::unique_ptr<AudioEncoder> MakeLdacEncoder();
std::unique_ptr<AudioEncoder> MakeSbcEncoder();

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
