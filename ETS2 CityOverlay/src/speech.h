#pragma once

#include "config.h"

#include <string>
#include <vector>

enum class SpeechPriority { Low = 0, Normal = 1, High = 2 };

// Speaks with fish.audio ([voice] engine=fish: e.g. your own cloned voice; tried twice) and, when
// that fails, the local natural voice (Piper, a small helper process next to the game at low
// priority); the Windows voice only if Piper isn't installed. Never blocks the caller.
void speech_start(const Config& cfg, const std::wstring& dir);
void speech_stop();

// key: a waiting message with the same key is replaced instead of queued twice.
// expire_s: drop the message if it couldn't be said in time ("exit in 500 m" is useless later).
// A High message interrupts a Low one that's playing (e.g. an alert over the tour guide).
// local_only: skip fish.audio (it already failed for this line).
void speech_say(const std::string& text, SpeechPriority priority = SpeechPriority::Normal,
                const std::string& key = std::string(), int expire_s = 30, bool local_only = false);
// The same with audio that's already made (a WAV file image, e.g. the co-driver's Google voice).
// `text` is only for the log.
void speech_play(std::vector<char> wav, const std::string& text, SpeechPriority priority = SpeechPriority::Low,
                 const std::string& key = std::string(), int expire_s = 30);
void speech_set_paused(bool paused);  // hold messages while the game is paused
void speech_set_muted(bool muted);    // the plugin is switched off: drop everything, say nothing
// The overall voice level (0-100, on top of [voice] volume / local_volume). change_level adds
// `delta`, says the new level and saves it in the settings; returns it.
int  speech_level();
int  speech_change_level(int delta);
bool speech_available();              // false when speech is off or no engine could start
