#include "svc.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

// ---- minimal JSON reader (objects, arrays, strings, numbers, literals) ----

namespace {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json *Get(const std::string &key) const
    {
        auto it = obj.find(key);
        return type == Object && it != obj.end() ? &it->second : nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string &text) : t_(text) {}

    bool Parse(Json &out)
    {
        return Value(out) && (Ws(), i_ == t_.size());
    }

private:
    void Ws()
    {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\r' || t_[i_] == '\n'))
            i_++;
    }

    bool Lit(const char *word)
    {
        size_t n = strlen(word);
        if (t_.compare(i_, n, word) != 0)
            return false;
        i_ += n;
        return true;
    }

    bool Str(std::string &out)
    {
        if (t_[i_] != '"')
            return false;
        for (i_++; i_ < t_.size(); i_++) {
            char c = t_[i_];
            if (c == '"') {
                i_++;
                return true;
            }
            if (c == '\\' && i_ + 1 < t_.size()) {
                char e = t_[++i_];
                if (e == 'u') {  // keep \uXXXX escapes as-is: only ASCII keys/values matter here
                    out += "\\u";
                    continue;
                }
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                continue;
            }
            out += c;
        }
        return false;
    }

    bool Value(Json &v)
    {
        Ws();
        if (i_ >= t_.size())
            return false;
        char c = t_[i_];
        if (c == '{') {
            v.type = Json::Object;
            i_++;
            Ws();
            if (t_[i_] == '}')
                return i_++, true;
            for (;;) {
                std::string key;
                Ws();
                if (!Str(key))
                    return false;
                Ws();
                if (t_[i_++] != ':')
                    return false;
                if (!Value(v.obj[key]))
                    return false;
                Ws();
                if (t_[i_] == ',') {
                    i_++;
                    continue;
                }
                return t_[i_++] == '}';
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            i_++;
            Ws();
            if (t_[i_] == ']')
                return i_++, true;
            for (;;) {
                v.arr.emplace_back();
                if (!Value(v.arr.back()))
                    return false;
                Ws();
                if (t_[i_] == ',') {
                    i_++;
                    continue;
                }
                return t_[i_++] == ']';
            }
        }
        if (c == '"')
            return v.type = Json::String, Str(v.s);
        if (Lit("true"))
            return v.type = Json::Bool, v.b = true, true;
        if (Lit("false"))
            return v.type = Json::Bool, v.b = false, true;
        if (Lit("null"))
            return v.type = Json::Null, true;
        char *end = nullptr;
        v.n = strtod(t_.c_str() + i_, &end);
        if (end == t_.c_str() + i_)
            return false;
        v.type = Json::Number;
        i_ = end - t_.c_str();
        return true;
    }

    const std::string &t_;
    size_t i_ = 0;
};

bool Flag(const Json &dev, const char *key, bool fallback)
{
    const Json *v = dev.Get(key);
    return v && v->type == Json::Bool ? v->b : fallback;
}

std::string Text(const Json &dev, const char *key, const char *fallback)
{
    const Json *v = dev.Get(key);
    return v && v->type == Json::String ? v->s : fallback;
}

}  // namespace

std::string DataRoot()
{
    const char *pd = getenv("ProgramData");
    return std::string(pd ? pd : "C:\\ProgramData") + "\\LdacA2dp";
}

bool LoadSettings(const std::string &address, DeviceSettings &out, std::string &note)
{
    out = DeviceSettings();
    std::ifstream f(DataRoot() + "\\settings.json", std::ios::binary);
    if (!f) {
        note = "no settings.json, using defaults";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF)
        text.erase(0, 3);  // UTF-8 BOM

    Json root;
    if (!Parser(text).Parse(root)) {
        note = "settings.json is not valid JSON, using defaults";
        return false;
    }
    const Json *devices = root.Get("devices");
    const Json *dev = devices ? devices->Get(address) : nullptr;
    if (!dev) {
        note = "no settings for " + address + ", using defaults";
        return false;
    }

    out.ldac = Text(*dev, "codec", "LDAC") != "SBC";
    out.freqMask = 0;
    if (Flag(*dev, "rate44", true)) out.freqMask |= LDAC_FREQ_44100;
    if (Flag(*dev, "rate48", true)) out.freqMask |= LDAC_FREQ_48000;
    if (Flag(*dev, "rate88", true)) out.freqMask |= LDAC_FREQ_88200;
    if (Flag(*dev, "rate96", true)) out.freqMask |= LDAC_FREQ_96000;
    out.dualChannel = Text(*dev, "channelMode", "Stereo") == "Dual Channel";
    out.cmMask = 0;
    if (Flag(*dev, "allowStereo", true))
        out.cmMask |= out.dualChannel ? LDAC_CM_DUAL : LDAC_CM_STEREO;
    if (Flag(*dev, "allowMono", false))
        out.cmMask |= LDAC_CM_MONO;
    out.bits24 = Flag(*dev, "bits24", true);
    std::string q = Text(*dev, "quality", "SQ");
    out.eqmid = q == "HQ" ? 0 : q == "MQ" ? 2 : 1;
    out.adaptive = Flag(*dev, "adaptive", true);
    if (!out.freqMask || !out.cmMask) {
        note = "settings allow no rate or channel mode, using defaults";
        out = DeviceSettings();
        return false;
    }
    note = "settings loaded for " + address;
    return true;
}

a2dp_prefs ToPrefs(const DeviceSettings &s)
{
    a2dp_prefs p;
    a2dp_prefs_default(&p);
    p.allow_ldac = s.ldac;
    p.ldac_freq_mask = s.freqMask;
    p.ldac_cm_mask = s.cmMask;
    // Prefer the highest rate the user allowed, as Alternative A2DP does.
    p.ldac_pref_rate = (s.freqMask & LDAC_FREQ_96000) ? 96000
                     : (s.freqMask & LDAC_FREQ_88200) ? 88200
                     : (s.freqMask & LDAC_FREQ_48000) ? 48000 : 44100;
    return p;
}

std::string ChannelModeName(uint8_t cm)
{
    return cm == LDAC_CM_MONO ? "Mono" : cm == LDAC_CM_DUAL ? "Dual Channel" : "Stereo";
}

static std::string StatusFile(const std::string &address)
{
    std::string name = address;
    for (char &c : name)
        if (c == ':')
            c = '-';
    return DataRoot() + "\\status\\" + name + ".json";
}

void WriteStatus(const std::string &address, const LinkInfo &link, int bitDepth, int kbps)
{
    CreateDirectoryA(DataRoot().c_str(), nullptr);
    CreateDirectoryA((DataRoot() + "\\status").c_str(), nullptr);
    std::string path = StatusFile(address), tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "{ \"codec\": \"%s\", \"channelMode\": \"%s\", \"sampleRate\": %u, \"bitDepth\": %d, \"bitrateKbps\": %d }\n",
            link.codec.c_str(), ChannelModeName(link.ldacCm).c_str(), link.sampleRate, bitDepth, kbps);
    fclose(f);
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void ClearStatus(const std::string &address)
{
    DeleteFileA(StatusFile(address).c_str());
}

uint64_t ParseAddress(const std::string &text)
{
    uint64_t a = 0;
    int digits = 0;
    for (char c : text) {
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            continue;
        a = a << 4 | (uint64_t)v;
        digits++;
    }
    return digits == 12 ? a : 0;
}

std::string FormatAddress(uint64_t a)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(a >> 40 & 0xFF), (unsigned)(a >> 32 & 0xFF),
             (unsigned)(a >> 24 & 0xFF), (unsigned)(a >> 16 & 0xFF), (unsigned)(a >> 8 & 0xFF), (unsigned)(a & 0xFF));
    return buf;
}
