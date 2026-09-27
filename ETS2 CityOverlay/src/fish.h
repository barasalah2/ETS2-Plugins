#pragma once

#include "config.h"

#include <string>
#include <vector>

// fish.audio text to speech (e.g. the driver's own cloned voice), used for everything the plugin
// says; the local voice (Piper) takes over when it fails. Blocking: call from a worker thread.
struct FishVoice
{
    std::string key;    // API key (never logged); empty = not set up
    std::string voice, model;
    double temperature = 0.7, top_p = 0.7, speed = 1.0;
    int sample_rate = 24000;  // small files, quick answers; plenty for a voice

    bool usable() const { return !key.empty() && !voice.empty(); }
    std::string cache_key(const std::string& text) const;  // same text + same voice = same file
};

// The settings from [voice] plus the key from fish_audio_key.txt (or FISH_AUDIO_API_KEY).
FishVoice fish_voice(const Config& cfg, const std::wstring& dir);

enum class FishResult { Ok, Failed, Refused };  // Refused: bad key, no credit or a wrong voice - stop trying

// A WAV file image (sizes fixed) of `text`, or why not. timeout_ms caps the whole request.
FishResult fish_speak(const FishVoice& v, const std::string& text, int timeout_ms, std::vector<char>& wav,
                      std::string& why);
