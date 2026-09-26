#pragma once

// Wording helpers for things the voice says: distances and times the way a person would say them.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

inline std::string spoken_km(double km)
{
    char b[48];
    if (km < 0.95) {
        const int m = std::max(100, (int)std::lround(km * 10.0) * 100);
        snprintf(b, sizeof(b), "%d metres", m);
    } else if (km < 1.5) {
        snprintf(b, sizeof(b), "1 kilometre");
    } else {
        snprintf(b, sizeof(b), "%d kilometres", (int)std::lround(km));
    }
    return b;
}

inline std::string spoken_minutes(double minutes)
{
    const int m = std::max(1, (int)std::lround(minutes));
    const int h = m / 60, r = m % 60;
    char b[64];
    if (h == 0) snprintf(b, sizeof(b), "%d minute%s", m, m == 1 ? "" : "s");
    else if (r < 5) snprintf(b, sizeof(b), "%d hour%s", h, h == 1 ? "" : "s");
    else snprintf(b, sizeof(b), "%d hour%s %d minutes", h, h == 1 ? "" : "s", r);
    return b;
}

// Countries that take "the" in English.
inline std::string spoken_country(const std::string& name)
{
    for (const char* c : {"Czech Republic", "United Kingdom", "Netherlands"})
        if (name == c) return "the " + name;
    return name;
}

// "80,70" (truck limits on motorways, other roads) -> a sentence, or "" if unknown.
inline std::string spoken_limits(const std::string& detail)
{
    int motorway = 0, local = 0;
    if (sscanf(detail.c_str(), "%d,%d", &motorway, &local) != 2 || motorway <= 0) return {};
    char b[128];
    if (local <= 0 || local == motorway)
        snprintf(b, sizeof(b), "The truck speed limit is %d kilometres per hour.", motorway);
    else
        snprintf(b, sizeof(b), "Truck speed limits are %d on motorways and %d on other roads.", motorway, local);
    return b;
}

inline std::string ordinal(int n)
{
    static const char* words[] = {"", "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth"};
    if (n >= 1 && n <= 8) return words[n];
    return std::to_string(n) + "th";
}

// A junction maneuver (see Turn in route.h) as an instruction: "take the exit on the right".
inline std::string spoken_turn(int turn, int flags, int exit_no)
{
    const bool exit = (flags & 4) != 0;  // leaving a road that carries on
    switch (turn) {
        case 1: return exit ? "take the exit on the left" : "keep left";
        case 11: return exit ? "take the exit on the right" : "keep right";
        case 2: return "turn left";
        case 12: return "turn right";
        case 3: return "turn sharp left";
        case 13: return "turn sharp right";
        case 4: case 14: return "make a U-turn";
        default: break;
    }
    if (turn >= 21 && turn <= 28)
        return exit_no > 0 ? "at the roundabout, take the " + ordinal(exit_no) + " exit" : "go round the roundabout";
    return {};
}

// Which lanes to use, from the lane mask (bit 0 = lane closest to the centre divider).
// Returns "" when every lane works or there's only one.
inline std::string spoken_lanes(int lanes, unsigned mask, bool left_hand_traffic)
{
    if (lanes < 2 || lanes > 16) return {};
    bool ok[16] = {};
    int n = 0, first = -1, last = -1;
    for (int i = 0; i < lanes; ++i) {  // i: 0 = leftmost lane as you look ahead
        const int bit = left_hand_traffic ? lanes - 1 - i : i;
        ok[i] = (mask >> bit) & 1u;
        if (ok[i]) {
            ++n;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (n == 0 || n == lanes) return {};
    if (last - first + 1 != n) return "Check your lane.";
    if (last == lanes - 1) return n == 1 ? "Use the right lane." : n == 2 ? "Use the two right lanes." : "Keep to the right lanes.";
    if (first == 0) return n == 1 ? "Use the left lane." : n == 2 ? "Use the two left lanes." : "Keep to the left lanes.";
    return n == 1 && lanes == 3 ? "Use the middle lane." : "Use the middle lanes.";
}

// "Truck stop" -> "truck stop" for use mid-sentence.
inline std::string spoken_place(std::string label)
{
    if (!label.empty()) label[0] = (char)std::tolower((unsigned char)label[0]);
    return label;
}
