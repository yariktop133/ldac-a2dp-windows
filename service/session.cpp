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
#include <deque>

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

    FILE *pcmDump = nullptr, *pcmIndex = nullptr;
    if (!o.pcmDumpPath.empty()) {
        pcmDump = fopen(o.pcmDumpPath.c_str(), "wb");
        pcmIndex = fopen((o.pcmDumpPath + ".txt").c_str(), "w");
    }

    // source
    std::unique_ptr<AudioSource> src;
    if (o.source.rfind("wav:", 0) == 0)
        src = MakeWavSource(o.source.substr(4));
    else if (o.source == "endpoint" && o.sinkKind == "driver")
        src = MakeEndpointSource(addr);
    else
        src = MakeLoopbackSource(o.source.rfind("loopback:", 0) == 0 ? o.source.substr(9) : "");
    if (!src->Start(err)) {
        if (dump)
            fclose(dump);
        return err = "source: " + err, SessionEnd::Error;
    }
    printf("source: %s\n", src->Describe().c_str());
    fflush(stdout);

    uint32_t srcRate = src->Rate();
    auto rs = std::make_unique<Resampler>(srcRate, info.sampleRate, src->Channels());
    std::vector<float> in, resampled, mapped;
    std::vector<std::vector<uint8_t>> packets;

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now(), lastSound = start, nextTick = start, nextCheck = start + std::chrono::seconds(1);
    auto nextReport = start + std::chrono::seconds(o.reportSeconds);
    bool streaming = false;
    SessionEnd end = SessionEnd::Stopped;
    unsigned long long sent = 0, dropped = 0, sentBytes = 0, sentAtReport = 0, bytesAtReport = 0;
    double reportStart = 0;
    // per report: longest wait between two blocks of audio while streaming (a
    // starved link stutters even with no packet dropped) and the deepest TX queue
    auto lastData = start;
    // Warm-up after START: the link may barely move for ~500 ms right after it
    // (seen on hardware: 5 of 33 packets through in the first 450 ms, then a
    // burst), which the sink plays as a stutter and ABR answers by dropping to
    // MQ. So silence goes out until the link keeps pace (70 % of the packets of
    // the last 100 ms delivered; at least 300 ms, at most 2 s), never more than
    // 3 packets deep so it adds no latency; then the encoder gets back to the
    // chosen quality.
    struct Mark {
        Clock::time_point t;
        unsigned long long made, done;
    };
    std::deque<Mark> marks;
    bool warming = false;
    auto warmFrom = start;
    unsigned long long made = 0;  // packets encoded, sent or (warm-up silence) skipped
    size_t fadeLeft = 0, fadeLen = 0;  // fade-in after the warm-up: audio cut in mid-sound clicks
    auto startedAt = start, nextTrace = start;  // -v: queue trace for 3 s after START
    double maxGapMs = 0;
    unsigned maxQueue = 0;

    while (!stop) {
        in.clear();
        if (!src->Read(in, 5)) {
            end = SessionEnd::Finished;
            break;
        }
        auto now = Clock::now();
        if (pcmDump && !in.empty()) {
            fwrite(in.data(), sizeof(float), in.size(), pcmDump);
            fprintf(pcmIndex, "%.1f %zu %d %d %u\n", std::chrono::duration<double, std::milli>(now - start).count(),
                    in.size() / 2, streaming ? 1 : 0, warming ? 1 : 0, src->Rate());
        }
        if (!in.empty()) {
            if (streaming)
                maxGapMs = std::max(maxGapMs, std::chrono::duration<double, std::milli>(now - lastData).count());
            lastData = now;
        }

        float peak = 0;
        for (float v : in)
            peak = std::max(peak, std::fabs(v));
        if (peak > 1e-4f || (src->DataMeansSound() && !in.empty()))
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
            warming = true;
            warmFrom = startedAt = nextTrace = Clock::now();
            marks.clear();
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

        if (src->Rate() != srcRate) {  // endpoint stream reopened in another format
            srcRate = src->Rate();
            rs = std::make_unique<Resampler>(srcRate, info.sampleRate, src->Channels());
        }
        if (streaming && !in.empty()) {
            if (warming) {
                std::fill(in.begin(), in.end(), 0.0f);
            } else if (fadeLeft) {
                int ch = src->Channels();
                for (size_t i = 0; i + ch <= in.size() && fadeLeft; i += ch, fadeLeft--) {
                    float g = 1.0f - (float)fadeLeft / (float)fadeLen;
                    for (int c = 0; c < ch; c++)
                        in[i + c] *= g;
                }
            }
            resampled.clear();
            rs->Process(in.data(), in.size() / src->Channels(), resampled);
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
                made++;
                if (warming && sink->QueueDepth() >= 3)
                    continue;  // silence nobody misses; queueing it would only add delay
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
        maxQueue = std::max(maxQueue, sink->QueueDepth());
        if (o.verbose && streaming && now >= nextTrace && now - startedAt < std::chrono::seconds(3)) {
            printf("  +%4lld ms queue %2u sent %llu\n",
                   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt).count(),
                   sink->QueueDepth(), sent);
            nextTrace = now + std::chrono::milliseconds(50);
        }
        if (warming) {
            marks.push_back({ now, made, sink->Completed() });
            while (marks.size() > 1 && marks[1].t <= now - std::chrono::milliseconds(100))
                marks.pop_front();
            const Mark &m = marks.front();
            unsigned long long dm = made - m.made, dd = sink->Completed() - m.done;
            bool flowing = now - m.t >= std::chrono::milliseconds(100) && dm >= 3 && dd * 10 >= dm * 7;
            if ((flowing && now - warmFrom >= std::chrono::milliseconds(300)) || now - warmFrom >= std::chrono::seconds(2)) {
                warming = false;
                enc->ResetQuality();
                fadeLen = fadeLeft = src->Rate() * 30 / 1000;
                if (o.verbose)
                    printf("warm-up %lld ms%s\n",
                           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - warmFrom).count(),
                           flowing ? "" : " (link still slow)");
            }
        }

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
            if (o.verbose)
                printf("   gap %3.0f ms, max queue %2u\n", maxGapMs, maxQueue);
            maxGapMs = 0;
            maxQueue = 0;
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
    if (pcmDump)
        fclose(pcmDump);
    if (pcmIndex)
        fclose(pcmIndex);
    ClearStatus(address);
    printf("session: %s, %llu packets sent, %llu dropped%s%s\n", SessionEndName(end), sent, dropped,
           err.empty() ? "" : ": ", err.c_str());
    fflush(stdout);
    return end;
}
