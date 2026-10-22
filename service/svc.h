// ldacsvc: user-mode encoder service for the LdacA2dp driver.
//
//   audio source (WASAPI loopback / WAV) -> resampler -> libldac (+ABR)
//   -> A2DP media packets -> sink (LdacA2dp driver, or a simulated link)
//
// Settings come from %ProgramData%\LdacA2dp\settings.json (written by
// ui/LdacConfig), live state goes to %ProgramData%\LdacA2dp\status\<addr>.json
// (shown by the same UI). See docs/settings-format.md.
#pragma once

#include <atomic>
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
// Settings that need a new stream configuration (codec, rates, channel modes) when they change.
bool SameLinkPrefs(const DeviceSettings &a, const DeviceSettings &b);
// Last write time of settings.json, 0 if there is none.
long long SettingsStamp();

struct LinkInfo {
    std::string codec;      // "LDAC" or "SBC"
    uint32_t sampleRate = 0;
    int channels = 2;
    uint8_t ldacCm = LDAC_CM_STEREO;
    int mtu = 679;
    uint16_t delay100us = 0;
    a2dp_sbc sbc{};         // single-choice SBC configuration when codec == "SBC"
};

// Whether a stream configured elsewhere (by the driver at device start, by the
// headphones) is acceptable for these preferences. SBC always is: we only get it
// when LDAC is off or the sink has no LDAC.
bool ConfigMatches(const a2dp_prefs &prefs, const LinkInfo &link);

void WriteStatus(const std::string &address, const LinkInfo &link, int bitDepth, int kbps);
void ClearStatus(const std::string &address);
std::string ChannelModeName(uint8_t cm);

// ---- audio sources (capture.cpp) ------------------------------------------

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual bool Start(std::string &err) = 0;
    virtual uint32_t Rate() const = 0;  // may change after Read() (driver endpoint)
    virtual int Channels() const = 0;
    virtual std::string Describe() const = 0;
    // Appends interleaved float frames that became available, waiting up to
    // timeoutMs. Returns false at the end of a finite source.
    virtual bool Read(std::vector<float> &out, int timeoutMs) = 0;
    // Called after the stream (re)starts, which can block for a moment: forget
    // the backlog that built up meanwhile instead of bursting it into the link.
    virtual void Resync() {}
    // True when data arrives only while something plays (the driver endpoint):
    // then any data counts as sound, however quiet. Otherwise (loopback, files)
    // the stream starts on a signal above the silence threshold.
    virtual bool DataMeansSound() const { return false; }
    // Whether the producer keeps its stream open (the endpoint: an application
    // has it open, even paused). Polled on every pass, keep it cheap.
    virtual bool StreamOpen() { return false; }
};

std::unique_ptr<AudioSource> MakeLoopbackSource(const std::string &endpointMatch);
std::unique_ptr<AudioSource> MakeWavSource(const std::string &path);
// What applications play to the driver's "LDAC A2DP" endpoint (sinks.cpp).
std::unique_ptr<AudioSource> MakeEndpointSource(uint64_t address);

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
    // Every kAbrIntervalMs. Not suspended right after START: the link may stall
    // for ~250 ms, and holding the bit rate through it overflowed the queue on
    // hardware (dropped packets), while ABR stepping down drops nothing.
    virtual void Tick(unsigned txQueueDepth) { (void)txQueueDepth; }
    // Back to the chosen quality with a fresh ABR state, once the start-up
    // stall is over (see the warm-up in session.cpp).
    virtual void ResetQuality() {}
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
    virtual unsigned long long Completed() const = 0;          // packets the link has taken
    virtual uint16_t Delay100us() { return 0; }
    virtual std::string Describe() const = 0;
    // Polled on every pass of the session loop: false (with the reason) once the
    // stream or the device is gone.
    virtual bool Alive(std::string &why) { (void)why; return true; }
    // Tear the stream down so the next Open negotiates it again.
    virtual void Disconnect() {}
};

// address 0: the first LdacA2dp device found
std::unique_ptr<MediaSink> MakeDriverSink(uint64_t address);
// AVDTP state (avdtp_src_state) of the LdacA2dp device, -1 if there is none.
int DriverLinkState(uint64_t address);
// Simulated radio link: capacity in kbit/s over time ("0:1500,5:400").
std::unique_ptr<MediaSink> MakeNullSink(const std::string &linkSpec);

uint64_t ParseAddress(const std::string &text);
std::string FormatAddress(uint64_t address);

// ---- session (session.cpp) ------------------------------------------------------

struct SessionOptions {
    std::string address;            // empty: the first device
    std::string source = "endpoint";  // or loopback[:name], wav:file
    std::string sinkKind = "driver";
    std::string link = "0:2000";    // simulated link (--sink null)
    std::string dumpPath;
    std::string pcmDumpPath;        // debugging: raw float PCM from the source + <path>.txt block index
    double seconds = 0;             // 0: until stopped
    int reportSeconds = 1;
    bool watchSettings = true;      // pick up changes from the settings utility
    bool verbose = false;
};

enum class SessionEnd { Finished, Stopped, LinkLost, Reconfigure, Error };
const char *SessionEndName(SessionEnd e);
SessionEnd RunSession(const SessionOptions &o, const std::atomic<bool> &stop, std::string &err);

// ---- Windows service (winsvc.cpp) ---------------------------------------------

constexpr wchar_t kServiceName[] = L"LdacSvc";
int ServiceInstall(const std::string &args);  // args: session options for the service command line
int ServiceUninstall();
int ServiceDispatch(const SessionOptions &o);  // called by the SCM with --service
