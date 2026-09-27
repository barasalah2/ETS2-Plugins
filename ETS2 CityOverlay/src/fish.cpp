#include "fish.h"
#include "gemini.h"
#include "json.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

static std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

FishVoice fish_voice(const Config& cfg, const std::wstring& dir)
{
    FishVoice v;
    v.key = read_secret(dir, L"fish_audio_key.txt", "FISH_AUDIO_API_KEY");
    v.voice = narrow(cfg.fish_voice);
    v.model = narrow(cfg.fish_model);
    v.temperature = cfg.fish_temperature;
    v.top_p = cfg.fish_top_p;
    v.speed = std::clamp(cfg.fish_speed, 0.5, 2.0);
    return v;
}

std::string FishVoice::cache_key(const std::string& text) const
{
    char b[160];
    snprintf(b, sizeof(b), "%s|%s|%.2f|%.2f|%.2f|%d|", voice.c_str(), model.c_str(), temperature, top_p, speed,
             sample_rate);
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (const char* p = b; *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ULL;
    char out[24];
    snprintf(out, sizeof(out), "%016llx", (unsigned long long)h);
    return out;
}

// Streamed WAVs carry placeholder sizes (~4 GB); players need the real ones.
static void fix_sizes(std::vector<char>& wav)
{
    const uint32_t riff = (uint32_t)(wav.size() - 8);
    memcpy(&wav[4], &riff, 4);
    size_t pos = 12;
    while (pos + 8 <= wav.size()) {
        uint32_t len;
        memcpy(&len, &wav[pos + 4], 4);
        if (memcmp(&wav[pos], "data", 4) == 0) {
            const uint32_t real = (uint32_t)(wav.size() - pos - 8);
            if (len > real) memcpy(&wav[pos + 4], &real, 4);
            return;
        }
        if ((size_t)len > wav.size() - pos - 8) return;
        pos += 8 + len + (len & 1);
    }
}

FishResult fish_speak(const FishVoice& v, const std::string& text, int timeout_ms, std::vector<char>& wav,
                      std::string& why)
{
    wav.clear();
    why.clear();
    if (!v.usable()) {
        why = "not set up";
        return FishResult::Refused;
    }
    char tuning[200];
    snprintf(tuning, sizeof(tuning),
             ",\"format\":\"wav\",\"sample_rate\":%d,\"latency\":\"balanced\",\"normalize\":true,"
             "\"temperature\":%.2f,\"top_p\":%.2f",
             v.sample_rate, v.temperature, v.top_p);
    std::string body = "{\"text\":" + json_quote(text) + ",\"reference_id\":" + json_quote(v.voice) + tuning;
    if (std::fabs(v.speed - 1.0) > 0.01) {
        char speed[64];
        snprintf(speed, sizeof(speed), ",\"prosody\":{\"speed\":%.2f}", v.speed);
        body += speed;
    }
    body += "}";
    const std::string headers =
        "Authorization: Bearer " + v.key + "\r\nContent-Type: application/json\r\nmodel: " + v.model + "\r\n";
    std::string audio;
    const int status = https_post(L"api.fish.audio", L"/v1/tts", headers, body, audio, timeout_ms);
    if (status == 200 && audio.size() > 44 && memcmp(audio.data(), "RIFF", 4) == 0) {
        wav.assign(audio.begin(), audio.end());
        fix_sizes(wav);
        return FishResult::Ok;
    }
    why = status < 0 ? "no connection or too slow" : "HTTP " + std::to_string(status);
    std::string msg = audio.substr(0, 160);
    for (char& c : msg)
        if ((unsigned char)c < 32) c = ' ';
    if (status > 0 && !msg.empty()) why += ": " + msg;
    // A bad key, no credit, or a wrong voice or model won't fix itself.
    return (status == 400 || status == 401 || status == 402 || status == 403 || status == 404) ? FishResult::Refused
                                                                                               : FishResult::Failed;
}
