#pragma once

#include <string>
#include <vector>

// Push-to-talk on the co-driver key: a short tap is a plain "tell me about here", holding it
// records the microphone (16 kHz mono) until it's let go. The device is opened on a small thread
// of its own, so neither the game nor the render thread waits for it.
// record = false: never open the microphone, every press is a tap.
void mic_start(bool record);
void mic_stop();

// The key went down or up (any thread; call on changes).
void mic_hold(bool down);
bool mic_listening();  // recording right now (for the "Listening..." card)

struct MicResult
{
    bool tap = false;          // let go quickly, or no microphone: just "tell me about here"
    std::vector<char> wav;     // what was said, as a WAV file (empty for a tap)
};
// Takes the next finished press, if any (game thread).
bool mic_take(MicResult& out);
