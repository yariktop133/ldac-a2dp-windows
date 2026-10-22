// One streaming session: settings -> sink (negotiate) -> encoder -> source,
// then the capture/encode/send loop until the source ends, the caller stops it,
// the link goes away or the settings need a new stream configuration.
#include "svc.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

const char *SessionEndName(SessionEnd e)
{
    switch (e) {
    case SessionEnd::Finished: return "source finished";
    case SessionEnd::Stopped: return "stopped";
    case SessionEnd::LinkLost: return "link lost";
    case SessionEnd::Reconfigure: return "settings changed";
    default: return "error";
    }
}

SessionEnd RunSession(const SessionOptions &o, const std::atomic<bool> &stop, std::string &err)
{
    uint64_t addr = ParseAddress(o.address);
    std::string address = o.address.empty() && o.sinkKind == "null" ? "00:00:00:00:00:00" : o.address;

    // settings -> preferences
    DeviceSettings settings;
    std::string note;
    long long settingsStamp = SettingsStamp();
    LoadSettings(address.empty() ? std::string() : FormatAddress(addr), settings, note);
    printf("settings: %s\n", note.c_str());
    a2dp_prefs prefs = ToPrefs(settings);

    // sink: negotiate the stream
    std::unique_ptr<MediaSink> sink = o.sinkKind == "null" ? MakeNullSink(o.link) : MakeDriverSink(addr);
    LinkInfo info;
    if (!sink->Open(prefs, info, err))
        return SessionEnd::Error;
    if (!ConfigMatches(prefs, info)) {
        // Configured before we got here (by the driver at device start, or by the
        // headphones) with preferences other than ours: renegotiate once.
        printf("link: %s %u Hz %s does not match the settings, reconnecting\n", info.codec.c_str(),
               info.sampleRate, ChannelModeName(info.ldacCm).c_str());
        sink->Disconnect();
        sink = MakeDriverSink(addr);
        if (!sink->Open(prefs, info, err))
            return SessionEnd::Error;
    }
    if (address.empty())
        address = sink->Describe().substr(sink->Describe().rfind(' ') + 1);
    else
        address = FormatAddress(addr);
    if (addr == 0)
        addr = ParseAddress(address);
    printf("link: %s, %s %u Hz %s, MTU %d, delay %.1f ms\n", sink->Describe().c_str(), info.codec.c_str(),
           info.sampleRate, ChannelModeName(info.ldacCm).c_str(), info.mtu, info.delay100us / 10.0);

    if (info.codec != "SBC" && info.codec != "LDAC")
        return err = "encoder: unsupported codec " + info.codec, SessionEnd::Error;
    auto makeEncoder = [&](std::unique_ptr<AudioEncoder> &enc) {
        enc = info.codec == "SBC" ? MakeSbcEncoder() : MakeLdacEncoder();
        if (!enc->Init(info, settings, err))
            return err = "encoder: " + err, false;
        printf("encoder: %s\n", enc->Describe().c_str());
        return true;
    };
    std::unique_ptr<AudioEncoder> enc;
    if (!makeEncoder(enc))
        return SessionEnd::Error;

    FILE *dump = nullptr;
    if (!o.dumpPath.empty() && !(dump = fopen(o.dumpPath.c_str(), "wb")))
        return err = "cannot create " + o.dumpPath, SessionEnd::Error;

    // source
    std::unique_ptr<AudioSource> src;
    if (o.source.rfind("wav:", 0) == 0)
        src = MakeWavSource(o.source.substr(4));
    else
        src = MakeLoopbackSource(o.source.rfind("loopback:", 0) == 0 ? o.source.substr(9) : "");
    if (!src->Start(err)) {
        if (dump)
            fclose(dump);
        return err = "source: " + err, SessionEnd::Error;
    }
    printf("source: %s\n", src->Describe().c_str());
    fflush(stdout);

    Resampler rs(src->Rate(), info.sampleRate, src->Channels());
    std::vector<float> in, resampled, mapped;
    std::vector<std::vector<uint8_t>> packets;

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now(), lastSound = start, nextTick = start, nextCheck = start + std::chrono::seconds(1);
    auto nextReport = start + std::chrono::seconds(o.reportSeconds);
    bool streaming = false;
    SessionEnd end = SessionEnd::Stopped;
    unsigned long long sent = 0, dropped = 0, sentBytes = 0, sentAtReport = 0, bytesAtReport = 0;
    double reportStart = 0;

    while (!stop) {
        in.clear();
        if (!src->Read(in, 5)) {
            end = SessionEnd::Finished;
            break;
        }
        auto now = Clock::now();

        float peak = 0;
        for (float v : in)
            peak = std::max(peak, std::fabs(v));
        if (peak > 1e-4f)
            lastSound = now;
        bool silent = now - lastSound > std::chrono::seconds(3);

        // START only while there is audio, SUSPEND after 3 s of silence (as the reference does).
        if (!streaming && !silent && !in.empty()) {
            if (!sink->Start(err)) {
                err = "sink: " + err;
                end = SessionEnd::LinkLost;
                break;
            }
            streaming = true;
            src->Resync();
            in.clear();  // this read predates START; the source resumes from here
            if (o.verbose)
                printf("START\n");
        } else if (streaming && silent) {
            sink->Suspend();
            streaming = false;
            if (o.verbose)
                printf("SUSPEND (silence)\n");
        }

        if (streaming && !in.empty()) {
            resampled.clear();
            rs.Process(in.data(), in.size() / src->Channels(), resampled);
            const float *frames = resampled.data();
            size_t count = resampled.size() / src->Channels();
            if (info.channels != src->Channels()) {  // stereo source, mono link
                mapped.resize(count);
                for (size_t i = 0; i < count; i++)
                    mapped[i] = 0.5f * (resampled[2 * i] + resampled[2 * i + 1]);
                frames = mapped.data();
            }
            packets.clear();
            enc->Encode(frames, count, packets);
            for (auto &p : packets) {
                if (dump && p.size() > 13)
                    fwrite(p.data() + 13, 1, p.size() - 13, dump);  // skip RTP (12) + payload header (1)
                if (sink->Send(p.data(), p.size())) {
                    sent++;
                    sentBytes += p.size();
                } else {
                    dropped++;
                }
            }
        }
        sink->Pump();

        if (now >= nextTick) {
            enc->Tick(sink->QueueDepth());
            nextTick += std::chrono::milliseconds(AudioEncoder::kAbrIntervalMs);
            if (nextTick < now)
                nextTick = now;
        }
        if (!sink->Alive(err)) {  // cheap, every pass: device removal must not wait
            end = SessionEnd::LinkLost;
            break;
        }
        if (now >= nextCheck) {
            nextCheck += std::chrono::seconds(1);
            WriteStatus(address, info, enc->BitDepth(), enc->NominalKbps());

            long long stamp = o.watchSettings ? SettingsStamp() : settingsStamp;
            if (stamp != settingsStamp) {
                settingsStamp = stamp;
                DeviceSettings fresh;
                LoadSettings(address, fresh, note);
                if (!SameLinkPrefs(fresh, settings)) {
                    printf("settings: %s, stream configuration changed\n", note.c_str());
                    sink->Disconnect();
                    end = SessionEnd::Reconfigure;
                    break;
                }
                // Same stream, new encoder parameters (quality, ABR, bit depth).
                settings = fresh;
                if (!makeEncoder(enc)) {
                    end = SessionEnd::Error;
                    break;
                }
            }
        }
        if (now >= nextReport) {
            double t = std::chrono::duration<double>(now - start).count(), span = t - reportStart;
            printf("%7.1fs %-9s %-4s %4d kbps | %4.0f pkt/s %5.0f kbit/s on air | queue %2u | dropped %llu\n", t,
                   streaming ? "streaming" : "suspended", enc->Quality().c_str(), enc->NominalKbps(),
                   (sent - sentAtReport) / span, (sentBytes - bytesAtReport) * 8 / 1000.0 / span,
                   sink->QueueDepth(), dropped);
            fflush(stdout);
            sentAtReport = sent;
            bytesAtReport = sentBytes;
            reportStart = t;
            nextReport += std::chrono::seconds(o.reportSeconds);
        }
        if (o.seconds > 0 && now - start > std::chrono::duration<double>(o.seconds))
            break;
    }

    if (streaming && end != SessionEnd::LinkLost && end != SessionEnd::Reconfigure)
        sink->Suspend();
    if (dump)
        fclose(dump);
    ClearStatus(address);
    printf("session: %s, %llu packets sent, %llu dropped%s%s\n", SessionEndName(end), sent, dropped,
           err.empty() ? "" : ": ", err.c_str());
    fflush(stdout);
    return end;
}
