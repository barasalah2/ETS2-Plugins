#pragma once

#include "config.h"

#include <string>
#include <vector>

// AI co-driver: Google Gemini (free tier) riding along. Each time something happens - a new job,
// a city, a border, a fine, a delivery, the driver pressing the key or talking, or just a quiet
// stretch of road - it gets the latest situation report (where, when, the job, what's ahead on
// the route), the conversation so far, and optionally a screenshot or the driver's voice, and
// answers in a sentence or two. The reply is shown on screen and read out in a Google voice (or
// the local one). Fuel, sleep and turn warnings are not its job; the navigation voice does those.
//
// Runs on its own thread; every function here is quick and safe to call from the game thread.
// Needs a Gemini API key (see gemini.h); without one it stays off.
void codriver_start(const Config& cfg, const std::wstring& dir);
void codriver_stop();

// The latest situation, about once a second: `report` is plain sentences for the model,
// `brief` a short "Tue 14:05, near Leoben, Austria" for the conversation history.
void codriver_set_situation(const std::string& report, const std::string& brief);

enum class Moment
{
    Talk,       // the driver spoke to it (audio)
    AskHere,    // the driver tapped the key: tell me about where I am
    Delivered,  // job delivered (detail: revenue, XP...)
    Fined,      // detail: what for and how much
    Ferry,      // boarding a ferry or train (detail: from/to)
    JobStart,   // a new job: a short briefing
    Border,     // about to enter a country (place = the country)
    City,       // entered a city
    Look,       // a regular look through the front camera: a word if the picture shows something useful
};

struct MomentInfo
{
    Moment kind = Moment::Look;
    std::string place_id, place, country;  // City / AskHere: the city (place_id for its fact cache)
    bool nearby = false;                   // AskHere: close to the place, not in it
    std::string detail;                    // what happened, in words
    std::vector<char> audio;               // Talk: the recording (WAV)
};
void codriver_moment(MomentInfo m);

// Real seconds since it last said something (large when it hasn't yet).
double codriver_quiet_seconds();
