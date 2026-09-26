#pragma once

#include <scssdk.h>

// Writes to the game's own log (Documents\Euro Truck Simulator 2\game.log.txt),
// prefixed with "[city_overlay]". Safe to call before init / after shutdown (no-op).
// Safe from any thread: messages from other threads are held until log_flush() runs on the
// game thread (the thread that called log_set_sink).
void log_set_sink(scs_log_t sink);
void log_flush();
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);
