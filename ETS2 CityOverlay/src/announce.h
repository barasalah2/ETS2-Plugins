#pragma once

#include "config.h"
#include "overlay.h"
#include "route.h"

#include <string>

// Decides what the voice says and when: fuel/sleep warnings (when they first appear or change),
// reminders as you approach the stop they name, ferries, border crossings and the destination.
// Each thing is said once per job route. Call a few times a second.
// speed_kmh: the speedometer; calls are timed in real seconds to the junction or stop.
// time_scale: game seconds per real second where the truck is (local.scale, ~19 on open road).
void announce_update(const Config& cfg, const RouteView& view, const AlertState& alerts, const std::string& target,
                     float speed_kmh, double time_scale);

// Real seconds until a point `km` game km ahead at this speed. The game compresses time as well as
// distance (local.scale, ~19), so at 80 km/h the GPS distance drops ~0.4 km every real second.
double seconds_to(double km, float speed_kmh, double time_scale);
