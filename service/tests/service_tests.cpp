// Unit tests for the encoder service building blocks (no audio devices, no driver).
#include "../svc.h"

#include <cmath>
#include <cstdio>

extern "C" {
#include "avdtp_media.h"
}

static int g_checks, g_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(cond)) {                                                                  \
            g_failures++;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                               \
    } while (0)

static const double kPi = 3.14159265358979323846;

static std::vector<float> Sine(double freq, double rate, size_t frames, int channels, double amp = 0.5)
{
    std::vector<float> v(frames * channels);
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++)
            v[i * channels + c] = (float)(amp * std::sin(2 * kPi * freq * i / rate));
    return v;
}

// SNR of a resampled sine against the ideal sine at the output rate.
static double ResampleSnr(double inRate, double outRate, double freq)
{
    Resampler rs(inRate, outRate, 2);
    std::vector<float> in = Sine(freq, inRate, (size_t)inRate, 2), out;
    // feed in uneven chunks, like the capture loop does
    for (size_t pos = 0; pos < in.size() / 2;) {
        size_t n = std::min<size_t>(37 + pos % 500, in.size() / 2 - pos);
        rs.Process(in.data() + pos * 2, n, out);
        pos += n;
    }
    double sig = 0, noise = 0;
    size_t frames = out.size() / 2;
    for (size_t i = frames / 4; i < frames * 3 / 4; i++) {
        double ideal = 0.5 * std::sin(2 * kPi * freq * i / outRate);
        sig += ideal * ideal;
        noise += (out[2 * i] - ideal) * (out[2 * i] - ideal) + (out[2 * i + 1] - ideal) * (out[2 * i + 1] - ideal) / 1.0;
    }
    noise /= 2;
    return 10 * std::log10(sig / (noise > 0 ? noise : 1e-30));
}

static void TestResampler()
{
    double up = ResampleSnr(48000, 96000, 1000);
    double cd = ResampleSnr(44100, 96000, 3000);
    double down = ResampleSnr(96000, 48000, 1000);
    double same = ResampleSnr(48000, 48000, 1000);
    printf("resampler SNR: 48->96k %.1f dB, 44.1->96k %.1f dB, 96->48k %.1f dB, 48->48k %.1f dB\n", up, cd, down, same);
    CHECK(up > 60);
    CHECK(cd > 60);
    CHECK(down > 60);
    CHECK(same > 100);

    // length: one second in, about one second out
    Resampler rs(44100, 96000, 1);
    std::vector<float> in(44100, 0.0f), out;
    rs.Process(in.data(), in.size(), out);
    CHECK(out.size() > 95900 && out.size() <= 96000);
}

static void TestSettings()
{
    // As written by ui/LdacConfig (System.Text.Json, camelCase, string enums).
    const char *json = "\xEF\xBB\xBF{\r\n  \"devices\": {\r\n    \"12:34:56:78:9A:BC\": {\r\n"
                       "      \"driver\": \"Ldac\",\r\n      \"codec\": \"LDAC\",\r\n      \"allowMono\": true,\r\n"
                       "      \"allowStereo\": true,\r\n      \"rate44\": false,\r\n      \"rate48\": true,\r\n"
                       "      \"rate88\": false,\r\n      \"rate96\": true,\r\n      \"bits16\": true,\r\n"
                       "      \"bits24\": true,\r\n      \"channelMode\": \"Dual Channel\",\r\n"
                       "      \"quality\": \"HQ\",\r\n      \"adaptive\": false\r\n    },\r\n"
                       "    \"66:77:88:99:AA:BB\": { \"codec\": \"SBC\", \"quality\": \"MQ\" }\r\n  }\r\n}\r\n";
    DeviceSettings s;
    std::string note;
    CHECK(ParseSettings(json, "12:34:56:78:9A:BC", s, note));
    CHECK(s.ldac);
    CHECK(s.freqMask == (LDAC_FREQ_48000 | LDAC_FREQ_96000));
    CHECK(s.cmMask == (LDAC_CM_DUAL | LDAC_CM_MONO));
    CHECK(s.eqmid == 0);
    CHECK(!s.adaptive);
    CHECK(s.bits24);
    a2dp_prefs p = ToPrefs(s);
    CHECK(p.ldac_pref_rate == 96000);
    CHECK(p.allow_ldac == 1);

    CHECK(ParseSettings(json, "66:77:88:99:AA:BB", s, note));
    CHECK(!s.ldac);
    CHECK(s.eqmid == 2);
    CHECK(!ParseSettings(json, "00:11:22:33:44:55", s, note));      // unknown device: defaults
    CHECK(s.ldac && s.eqmid == 1 && s.adaptive);
    CHECK(!ParseSettings("{ \"devices\": ", "12:34:56:78:9A:BC", s, note));  // truncated file
    CHECK(!ParseSettings("{\"devices\":{\"12:34:56:78:9A:BC\":{\"rate44\":false,\"rate48\":false,"
                         "\"rate88\":false,\"rate96\":false}}}", "12:34:56:78:9A:BC", s, note));  // no rates allowed
    CHECK(s.freqMask != 0);

    CHECK(ParseAddress("12:34:56:78:9A:BC") == 0x123456789ABCull);
    CHECK(ParseAddress("123456789ABC") == 0x123456789ABCull);
    CHECK(ParseAddress("12:34:56") == 0);
    CHECK(FormatAddress(0x123456789ABCull) == "12:34:56:78:9A:BC");
}

static uint32_t Be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void CheckStream(const std::vector<std::vector<uint8_t>> &packets, size_t mtu, uint32_t tsPerFrame,
                        uint8_t sync)
{
    CHECK(!packets.empty());
    uint32_t ts = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        const auto &p = packets[i];
        CHECK(p.size() <= mtu && p.size() > 13);
        CHECK(p[0] == 0x80 && (p[1] & 0x7F) == 96);
        CHECK(((p[2] << 8) | p[3]) == (int)(i & 0xFFFF));
        CHECK(Be32(p.data() + 4) == ts);
        CHECK(Be32(p.data() + 8) == 0);  // SSRC 0, as in the reference
        unsigned frames = p[12] & 0x0F;
        CHECK(frames >= 1);
        CHECK(p[13] == sync);
        ts += frames * tsPerFrame;
    }
}

static void TestLdac()
{
    LinkInfo link;
    link.codec = "LDAC";
    link.sampleRate = 96000;
    link.channels = 2;
    link.ldacCm = LDAC_CM_STEREO;
    link.mtu = 679;
    DeviceSettings s;  // SQ, ABR
    std::string err;
    auto enc = MakeLdacEncoder();
    CHECK(enc->Init(link, s, err));
    std::vector<float> pcm = Sine(1000, 96000, 96000, 2);
    std::vector<std::vector<uint8_t>> packets;
    enc->Encode(pcm.data(), 96000, packets);
    CheckStream(packets, 679, 128, 0xAA);
    // reference: 125 packets/s of 673 bytes with 3 frames each at SQ/96 kHz
    CHECK(packets.size() >= 123 && packets.size() <= 126);
    CHECK(packets[10].size() == 673 && (packets[10][12] & 0x0F) == 3);
    CHECK(enc->NominalKbps() == 660);
    CHECK(enc->Quality() == "SQ");

    // ABR never climbs above the chosen quality, and drops on a deep queue
    for (int i = 0; i < 400; i++)
        enc->Tick(0);
    CHECK(enc->NominalKbps() <= 660);
    for (int i = 0; i < 100; i++)
        enc->Tick(12);
    CHECK(enc->NominalKbps() < 660);
}

// ABR after a congested start (seen on hardware): it must climb back once the
// queue is empty, and a queue that never reaches 0 keeps it at the bottom
// (why DriverSink does not count the packet in flight).
static int AbrAfterDrop(unsigned steadyDepth)
{
    LinkInfo link;
    link.codec = "LDAC";
    link.sampleRate = 96000;
    link.channels = 2;
    link.ldacCm = LDAC_CM_STEREO;
    link.mtu = 679;
    DeviceSettings s;  // SQ + ABR
    std::string err;
    auto enc = MakeLdacEncoder();
    if (!enc->Init(link, s, err))
        return -1;
    std::vector<float> pcm = Sine(1000, 96000, 1920, 2);  // 20 ms per ABR tick
    std::vector<std::vector<uint8_t>> packets;
    for (int i = 0; i < 50; i++) {  // 1 s of a full queue
        enc->Encode(pcm.data(), 1920, packets);
        enc->Tick(30);
    }
    for (int i = 0; i < 3000; i++) {  // 60 s of a steady queue
        enc->Encode(pcm.data(), 1920, packets);
        enc->Tick(steadyDepth);
    }
    return enc->NominalKbps();
}

static void TestAbrRecovery()
{
    int empty = AbrAfterDrop(0), oneInFlight = AbrAfterDrop(1);
    printf("ABR after a congested start, 60 s later: queue 0 -> %d kbps, queue 1 -> %d kbps\n", empty, oneInFlight);
    CHECK(empty == 660);        // back to the chosen SQ, not above
    CHECK(oneInFlight == 330);  // never recovers: depth 1 is not "empty" for libldac
}

static void TestSbc()
{
    LinkInfo link;
    link.codec = "SBC";
    link.sampleRate = 48000;
    link.channels = 2;
    link.mtu = 679;
    link.sbc = { SBC_FREQ_48000, SBC_CM_JOINT, SBC_BLOCK_16, SBC_SUBBANDS_8, SBC_ALLOC_LOUDNESS, 2, 53 };
    DeviceSettings s;
    std::string err;
    auto enc = MakeSbcEncoder();
    CHECK(enc->Init(link, s, err));
    std::vector<float> pcm = Sine(1000, 48000, 48000, 2);
    std::vector<std::vector<uint8_t>> packets;
    enc->Encode(pcm.data(), 48000, packets);
    CheckStream(packets, 679, 128, 0x9C);
    // joint stereo, bitpool 53: 119-byte frames, 5 per packet within MTU 679
    CHECK(packets[3].size() == 13 + 5 * 119);
    CHECK(enc->NominalKbps() == 357);
    // SBC header: 48 kHz (0b11), 16 blocks (0b11), joint stereo (0b11), loudness, 8 subbands
    CHECK(packets[3][14] == 0xFD);
    CHECK(packets[3][15] == 53);

    link.sbc.max_bitpool = 250;  // a sink allowing more: we still cap at 53
    auto enc2 = MakeSbcEncoder();
    CHECK(enc2->Init(link, s, err));
    CHECK(enc2->Quality() == "bp53");
}

int main()
{
    TestResampler();
    TestSettings();
    TestLdac();
    TestAbrRecovery();
    TestSbc();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
