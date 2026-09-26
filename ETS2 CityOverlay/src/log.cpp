#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static std::atomic<scs_log_t> g_sink{nullptr};
static std::thread::id g_game_thread;

// Messages from other threads (route worker, render thread) wait here until the game thread
// flushes them: the game's log function is only documented for use from its own callbacks.
static std::mutex g_queue_mutex;
static std::vector<std::pair<scs_log_type_t, std::string>> g_queue;

void log_set_sink(scs_log_t sink)
{
    g_game_thread = std::this_thread::get_id();
    g_sink = sink;
}

void log_flush()
{
    scs_log_t sink = g_sink;
    std::vector<std::pair<scs_log_type_t, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        pending.swap(g_queue);
    }
    if (!sink) return;
    for (const auto& [type, text] : pending) sink(type, text.c_str());
}

static void write(scs_log_type_t type, const char* fmt, va_list args)
{
    scs_log_t sink = g_sink;
    if (!sink) return;
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[city_overlay] ");
    vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
    if (std::this_thread::get_id() == g_game_thread) {
        sink(type, buf);
    } else {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_queue.size() < 200) g_queue.emplace_back(type, buf);
    }
}

void log_info(const char* fmt, ...)  { va_list a; va_start(a, fmt); write(SCS_LOG_TYPE_message, fmt, a); va_end(a); }
void log_warn(const char* fmt, ...)  { va_list a; va_start(a, fmt); write(SCS_LOG_TYPE_warning, fmt, a); va_end(a); }
void log_error(const char* fmt, ...) { va_list a; va_start(a, fmt); write(SCS_LOG_TYPE_error, fmt, a); va_end(a); }
