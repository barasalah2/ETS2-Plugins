#include "codriver.h"
#include "capture.h"
#include "csv.h"
#include "fish.h"
#include "gemini.h"
#include "json.h"
#include "log.h"
#include "overlay.h"
#include "speech.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

static const ULONGLONG kMinGap = 5000;  // between text requests (Flash Lite's free tier allows 15 a minute)
static const size_t    kHistory = 10;   // exchanges kept in the conversation
static const size_t    kMaxNotes = 25;  // lasting things the driver told it

struct Pending
{
    MomentInfo m;
    ULONGLONG  at;
};

static std::thread             g_thread;
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static bool                    g_quit = false;
static bool                    g_wake = false;  // a new moment arrived
static std::deque<Pending>     g_pending;
static std::string             g_report, g_brief;
static std::atomic<bool>       g_done{false};
static std::atomic<ULONGLONG>  g_last_said{0};
static Config                  g_cfg;
static std::wstring            g_dir;

// Worker thread only.
struct Exchange { std::string user, model; };
static std::deque<Exchange> g_history;
static std::vector<std::string> g_notes;
static std::unordered_map<std::string, std::vector<std::string>> g_told;  // place id -> what it said about it
static std::string g_usage_day;
static int         g_usage = 0;
static std::string g_blocked_day;     // Gemini's own daily limit was reached on this day
static std::string g_voice_off_day;   // the Google voice's daily limit was reached on this day
static bool        g_voice_broken = false;
static FishVoice   g_fish;                    // [voice] fish.audio settings and key (never logged)
static bool        g_fish_off = false;        // fish.audio refused us for good this session
static bool        g_debug = false;           // ETS2_CITY_OVERLAY_DEBUG: log reports, keep the last picture and clip

static void save_debug(const wchar_t* name, const std::vector<char>& data)
{
    if (!g_debug || data.empty()) return;
    if (FILE* f = _wfopen((g_dir + L"\\" + name).c_str(), L"wb")) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
}

static const char* kSystem =
    "You are the truck's onboard AI assistant, like the voice assistant in a modern car, with a front camera. "
    "The driver is playing Euro Truck Simulator 2; treat everything as real.\n"
    "You are a system, not a person or a passenger: don't claim a body, feelings or a life of your own, and "
    "don't say \"we\" or \"us\". Speak to the driver as \"you\".\n"
    "Each message brings an image from the front camera (what the driver sees, including the dashboard and "
    "the navigation display), a short report (game time, location, the job and the route ahead) and what "
    "just happened; sometimes the driver's voice.\n"
    "The camera image is your main source. Look at it closely and use what it shows: road signs (speed "
    "limits, exits and directions, warnings, tolls, borders); the road ahead (road works, an accident, a "
    "traffic jam, a closed lane, a sharp bend, a steep hill); weather and visibility (rain, fog, snow, "
    "darkness, low sun); landmarks, bridges, buildings and views worth a word; warning lights and messages "
    "on the dashboard or the screen. Speak of it as the truck's own camera and displays; never call it a "
    "game, graphics or a screenshot.\n"
    "How to respond:\n"
    "- Clear, calm and to the point, like a good car assistant: one or two short sentences, under 35 words. "
    "Up to 80 words only when the driver asks for detail.\n"
    "- Your words are read aloud: plain spoken sentences only, no lists, markdown, emoji or symbols, and "
    "units in words.\n"
    "- Only say what you can actually see or know for sure. If a sign or text is too small to read, don't "
    "guess it. Read each sign on its own: a distance on a direction sign belongs to that sign's place, not to "
    "a speed limit or another sign. A speed limit sign applies from where it stands. Facts about places must "
    "be accurate and well established.\n"
    "- Numbers only from the image or the report. Never invent them.\n"
    "- No small talk, jokes, teasing, opinions or personal remarks. Never suggest alcohol.\n"
    "- Don't repeat what was already said in the conversation, and don't describe the same view twice.\n"
    "- Don't give turn-by-turn directions or fuel and rest advice unprompted: the navigation handles those.\n"
    "- For automatic looks, speak only when the image shows something new and useful; otherwise leave "
    "\"say\" empty. Silence is normal.\n"
    "- Answer in the language the driver speaks.\n"
    "Reply as JSON: \"heard\" = the driver's words when their voice is attached, else empty; \"say\" = your "
    "spoken response; \"style\" = how the voice should sound, a few words (e.g. \"calm, clear\"); "
    "\"remember\" = a lasting preference or fact the driver told you (their name, what information they "
    "want), else empty.";

static const char* kSchema =
    "{\"type\":\"OBJECT\",\"properties\":{"
    "\"heard\":{\"type\":\"STRING\",\"description\":\"The driver's words, when their voice is attached\"},"
    "\"say\":{\"type\":\"STRING\",\"description\":\"What you say out loud; empty to stay quiet\"},"
    "\"style\":{\"type\":\"STRING\",\"description\":\"How to say it, a few words\"},"
    "\"remember\":{\"type\":\"STRING\",\"description\":\"A lasting fact about the driver, or empty\"}},"
    "\"required\":[\"say\"],\"propertyOrdering\":[\"heard\",\"say\",\"style\",\"remember\"]}";

// --- helpers --------------------------------------------------------------------------------

static std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Text for the voice: no markdown leftovers, one line.
static std::string clean(const std::string& s)
{
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '*' || c == '#' || c == '_' || c == '`') continue;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

static std::string today()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return b;
}

// Google's free quotas reset at midnight Pacific time, so that's the day they're counted in.
// (UTC-8: in summer this turns an hour after the reset, which only means waiting a bit longer.)
static std::string quota_day()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 8ULL * 3600 * 10000000;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME t;
    FileTimeToSystemTime(&ft, &t);
    char b[16];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return b;
}

// A Gemini model the co-driver may use. Each has its own free allowance.
struct Model
{
    std::wstring name;
    std::string  spent_day;       // its daily limit was reached on this (quota) day
    bool         broken = false;  // gone or refuses our requests: skipped this session
    bool         no_audio = false;// refuses the driver's voice: skipped for those
    bool         thinking = true; // takes thinkingConfig
    ULONGLONG    busy_until = 0;  // per-minute limit or "high demand": leave it alone until then
};

static const char* name_of(Moment k)
{
    switch (k) {
        case Moment::Talk:      return "talk";
        case Moment::AskHere:   return "asked";
        case Moment::Delivered: return "delivered";
        case Moment::Fined:     return "fined";
        case Moment::Ferry:     return "ferry";
        case Moment::JobStart:  return "new job";
        case Moment::Border:    return "border";
        case Moment::City:      return "city";
        default:                return "look";
    }
}

static bool asked(Moment k) { return k == Moment::Talk || k == Moment::AskHere; }

static int rank(Moment k)  // lower goes first
{
    switch (k) {
        case Moment::Talk:      return 0;
        case Moment::AskHere:   return 1;
        case Moment::Delivered:
        case Moment::Fined:
        case Moment::Ferry:     return 2;
        case Moment::JobStart:
        case Moment::Border:    return 3;
        case Moment::City:      return 4;
        default:                return 5;
    }
}

// How long something stays worth talking about, ms.
static ULONGLONG lifetime(Moment k)
{
    switch (k) {
        case Moment::JobStart: return 180000;
        case Moment::Border:   return 120000;
        case Moment::Look:     return 20000;
        default:               return 90000;
    }
}

// Quiet time wanted since its last words before an automatic remark, ms.
static ULONGLONG gap_before(Moment k)
{
    switch (k) {
        case Moment::Talk:
        case Moment::AskHere:   return 0;
        case Moment::Delivered:
        case Moment::Fined:
        case Moment::Ferry:     return 15000;  // worth a word while it's fresh
        case Moment::Look:      return 45000;  // regular looks: not right after it spoke
        default:                return 30000;
    }
}

// --- files: notes, places, usage ---------------------------------------------------------------

static void load_files()
{
    for (const auto& row : read_csv(g_dir + L"\\guide_cache.csv"))
        if (row.size() >= 2 && !row[0].empty() && row[0] != "id") g_told[row[0]].push_back(row[1]);
    if (FILE* f = _wfopen((g_dir + L"\\codriver_notes.txt").c_str(), L"rb")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string s = clean(line);
            if (!s.empty()) g_notes.push_back(s);
        }
        fclose(f);
    }
    if (FILE* f = _wfopen((g_dir + L"\\codriver_usage.txt").c_str(), L"rb")) {
        char day[32] = "";
        int n = 0;
        if (fscanf(f, "%31s %d", day, &n) == 2) g_usage_day = day, g_usage = n;
        fclose(f);
    }
}

static void remember_place(const std::string& id, const std::string& text)
{
    if (id.empty()) return;
    g_told[id].push_back(text);
    const std::wstring path = g_dir + L"\\guide_cache.csv";
    const bool is_new = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (FILE* f = _wfopen(path.c_str(), L"ab")) {
        if (is_new) fputs("id,text\n", f);
        fprintf(f, "%s,%s\n", csv_escape(id).c_str(), csv_escape(text).c_str());
        fclose(f);
    }
}

static void remember_note(const std::string& note)
{
    const std::string s = clean(note);
    if (s.size() < 3) return;
    auto lower = [](std::string v) { for (char& c : v) c = (char)tolower((unsigned char)c); return v; };
    for (const auto& n : g_notes)
        if (lower(n) == lower(s)) return;
    g_notes.push_back(s);
    if (g_notes.size() > kMaxNotes) g_notes.erase(g_notes.begin());
    if (FILE* f = _wfopen((g_dir + L"\\codriver_notes.txt").c_str(), L"wb")) {
        for (const auto& n : g_notes) fprintf(f, "%s\n", n.c_str());
        fclose(f);
    }
    log_info("assistant: will remember \"%s\"", s.c_str());
}

static int requests_left()
{
    if (g_usage_day != today()) g_usage_day = today(), g_usage = 0;
    return g_cfg.guide_daily_limit - g_usage;
}

static void count_request()
{
    requests_left();  // rolls the day over
    ++g_usage;
    if (FILE* f = _wfopen((g_dir + L"\\codriver_usage.txt").c_str(), L"wb")) {
        fprintf(f, "%s %d\n", g_usage_day.c_str(), g_usage);
        fclose(f);
    }
}

// --- worker ---------------------------------------------------------------------------------

// Waits that shutdown can cut short. Returns true when it's time to quit.
static bool nap(ULONGLONG ms)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_quit; });
}

static std::string place_words(const MomentInfo& m)
{
    return m.place + (m.country.empty() || m.country == m.place ? "" : ", " + m.country);
}

static std::string already_told(const MomentInfo& m)
{
    auto it = g_told.find(m.place_id);
    if (it == g_told.end() || it->second.empty()) return {};
    std::string s = " You've told the driver about it before, so say something different:";
    const size_t from = it->second.size() > 4 ? it->second.size() - 4 : 0;
    for (size_t i = from; i < it->second.size(); ++i) s += " \"" + it->second[i] + "\"";
    return s;
}

static std::string instruction(const MomentInfo& m, bool image)
{
    switch (m.kind) {
        case Moment::Talk:
            return "The driver is speaking to you; their voice is attached. Put their words in \"heard\", then "
                   "respond to them.";
        case Moment::AskHere:
            return std::string("The driver asks what's around. Using the camera image first, tell them what's notable "
                               "in view (a sign, a landmark, the road, the weather)") +
                   (m.place.empty() ? std::string(", and one accurate fact about the region.")
                                    : std::string(", and one accurate fact about ") + place_words(m) + " (" +
                                          (m.nearby ? "nearby" : "you're in it") + ")." + already_told(m));
        case Moment::Delivered:
            return "The delivery is complete: " + m.detail + ". Confirm it in one sentence with the key numbers.";
        case Moment::Fined:
            return "A fine was just issued: " + m.detail + ". Note it in one short, neutral sentence.";
        case Moment::Ferry:
            return "Boarding " + m.detail + ". Say where the crossing goes and give one fact about the destination.";
        case Moment::JobStart:
            return "A new job has started. Give a short route briefing: the cargo, the destination, the distance and "
                   "estimated driving time, and any border or ferry on the way.";
        case Moment::Border:
            return "The route crosses into " + m.place + " in " + (m.detail.empty() ? std::string("a few km") : m.detail) +
                   ". Announce it with one useful or interesting fact about the country (a greeting in the local "
                   "language is fine). Don't mention speed limits.";
        case Moment::City:
            return "Entering " + place_words(m) +
                   ". Say one notable thing about it: something visible in the camera image, or one accurate fact "
                   "about the city." + already_told(m);
        default:
            if (!image) return "Regular check, but the camera image is missing: leave \"say\" empty.";
            return "Regular camera check while driving. If the image shows something new and useful (a sign, road "
                   "works, a hazard or a jam, a change in weather or visibility, a notable landmark or view, or a "
                   "warning on the dashboard or screen), tell the driver in one short sentence. If nothing new or "
                   "useful is visible, leave \"say\" empty.";
    }
}

// What the history keeps of this moment (the full report isn't repeated).
static std::string history_line(const MomentInfo& m, const std::string& brief, const std::string& heard)
{
    std::string s = "[" + brief + "] ";
    switch (m.kind) {
        case Moment::Talk:      return s + "Driver: \"" + heard + "\"";
        case Moment::AskHere:   return s + "Driver asked about " + (m.place.empty() ? "the region" : place_words(m));
        case Moment::Delivered: return s + "Delivered the load (" + m.detail + ")";
        case Moment::Fined:     return s + "Fined: " + m.detail;
        case Moment::Ferry:     return s + "Boarding " + m.detail;
        case Moment::JobStart:  return s + "New job started";
        case Moment::Border:    return s + "About to cross into " + m.place;
        case Moment::City:      return s + "Entered " + place_words(m);
        default:                return s + "Quiet stretch of road";
    }
}

struct Reply
{
    std::string heard, say, style, remember;
};

// The reply is JSON (responseSchema). Some models put scratch notes in a plain text part before
// the answer, so each part is tried from the last one back, and never read out raw.
static bool parse_reply(const Json& j, Reply& out, std::string& why)
{
    const Json& cand = j["candidates"][0];
    const std::string& block = j["promptFeedback"]["blockReason"].str;
    const std::string& finish = cand["finishReason"].str;
    if (!block.empty() || (finish != "STOP" && finish != "MAX_TOKENS")) {
        why = "declined (" + (block.empty() ? finish : block) + ")";
        return false;
    }
    const auto& parts = cand["content"]["parts"].arr;
    for (size_t i = parts.size(); i-- > 0;) {
        if (parts[i]["thought"].b) continue;
        Json r;
        if (!json_parse(parts[i]["text"].str, r) || r.type != Json::Object || r["say"].type != Json::String) continue;
        out.heard = clean(r["heard"].str);
        out.say = clean(r["say"].str);
        out.style = clean(r["style"].str);
        out.remember = clean(r["remember"].str);
        return true;
    }
    why = finish == "MAX_TOKENS" ? "answer was cut off" : "unreadable answer";
    return false;
}

// Reads `text` in the Google voice. False if that didn't work (then the local voice is used).
static bool google_voice(const std::string& key, const std::string& text, const std::string& style,
                         std::vector<char>& wav)
{
    const std::string body =
        "{\"model\":" + json_quote(narrow(g_cfg.guide_tts_model)) + ",\"store\":false,"
        "\"input\":[{\"type\":\"user_input\",\"content\":[{\"type\":\"text\",\"text\":" + json_quote(text) +
        ",\"annotations\":[{\"type\":\"speech_metadata\",\"style\":" +
        json_quote(style.empty() ? "calm and clear" : style) + "}]}]}],"
        "\"response_format\":{\"type\":\"audio\"},"
        "\"generation_config\":{\"speech_config\":[{\"voice\":" + json_quote(narrow(g_cfg.guide_voice)) + "}]}}";
    count_request();
    const ULONGLONG t0 = GetTickCount64();
    const GeminiReply r = gemini_post(L"/v1beta/interactions", body, key);
    if (!r.ok()) {
        if (r.status == 429 && r.daily_limit()) {
            g_voice_off_day = quota_day();
            log_info("assistant: the Google voice's free daily limit is used up; the local voice reads the replies until tomorrow");
        } else if (r.status == 400 || r.status == 404) {
            g_voice_broken = true;
            log_warn("assistant: the Google voice refused the request (%d: %s); check [guide] google_voice= and google_voice_model=. "
                     "Using the local voice.", r.status, r.message.c_str());
        } else {
            log_info("assistant: Google voice unavailable (%d%s%s); using the local voice for this one", r.status,
                     r.message.empty() ? "" : ": ", r.message.c_str());
        }
        return false;
    }
    for (const Json& step : r.json["steps"].arr)
        for (const Json& c : step["content"].arr) {
            if (c["type"].str != "audio" || c["data"].str.empty()) continue;
            std::vector<char> data;
            if (!base64_decode(c["data"].str, data)) continue;
            const std::string& mime = c["mime_type"].str;
            if (data.size() > 44 && memcmp(data.data(), "RIFF", 4) == 0) {
                wav = std::move(data);
            } else {  // raw 16-bit PCM ("audio/l16;rate=24000"): add a WAV header
                const size_t at = mime.find("rate=");
                const uint32_t rate = at != std::string::npos ? (uint32_t)atoi(mime.c_str() + at + 5) : 24000;
                wav.assign(44, 0);
                auto put32 = [&](size_t i, uint32_t v) { memcpy(&wav[i], &v, 4); };
                auto put16 = [&](size_t i, uint16_t v) { memcpy(&wav[i], &v, 2); };
                memcpy(&wav[0], "RIFF", 4);
                put32(4, (uint32_t)(36 + data.size()));
                memcpy(&wav[8], "WAVEfmt ", 8);
                put32(16, 16);
                put16(20, 1);
                put16(22, 1);
                put32(24, rate ? rate : 24000);
                put32(28, (rate ? rate : 24000) * 2);
                put16(32, 2);
                put16(34, 16);
                memcpy(&wav[36], "data", 4);
                put32(40, (uint32_t)data.size());
                wav.insert(wav.end(), data.begin(), data.end());
            }
            log_info("assistant: Google voice ready in %llu ms", GetTickCount64() - t0);
            save_debug(L"codriver_last.wav", wav);
            return true;
        }
    log_info("assistant: the Google voice sent no audio; using the local voice for this one");
    return false;
}

static void show(const std::string& title, const std::string& text, const std::string& status = {})
{
    if (!g_cfg.guide_show) return;
    GuideCard card;
    card.active = true;
    card.title = title;
    card.text = text;
    card.status = status;
    overlay_set_guide(card);
}

static std::string card_title(const MomentInfo& m, const Reply& r)
{
    switch (m.kind) {
        case Moment::Talk:
            if (!r.heard.empty()) return "\"" + (r.heard.size() > 60 ? r.heard.substr(0, 57) + "...\"" : r.heard + "\"");
            return "Assistant";
        case Moment::AskHere: return m.place.empty() ? "Assistant" : (m.nearby ? "Near " : "About ") + m.place;
        case Moment::City:    return "About " + m.place;
        case Moment::Border:  return "Next: " + m.place;
        default:              return "Assistant";
    }
}

// Reads the reply out. [guide] voice_engine=google: the Google voice. Otherwise the plugin's own
// voice: fish.audio (made here, on the assistant's thread, so a long reply never holds up a turn
// call in the speech queue), tried twice, and then the local voice.
static void speak(const std::string& key, const Reply& r, const MomentInfo& m)
{
    if (!g_cfg.guide_speak) return;
    const int expire = asked(m.kind) ? 120 : 90;
    std::vector<char> wav;
    if (g_cfg.guide_voice_engine == L"google") {
        const bool google = !g_cfg.guide_voice.empty() && !g_voice_broken && g_voice_off_day != quota_day() &&
                            requests_left() > 0;
        if (google && google_voice(key, r.say, r.style, wav))
            speech_play(std::move(wav), r.say, SpeechPriority::Low, "codriver", expire);
        else
            speech_say(r.say, SpeechPriority::Low, "codriver", expire, true);
        return;
    }
    if (g_cfg.voice_engine == L"fish" && g_fish.usable() && !g_fish_off) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const ULONGLONG t0 = GetTickCount64();
            std::string why;
            const FishResult res = fish_speak(g_fish, r.say, 10000, wav, why);
            if (res == FishResult::Ok) {
                log_info("assistant: fish.audio voice ready in %llu ms", GetTickCount64() - t0);
                save_debug(L"codriver_last.wav", wav);
                speech_play(std::move(wav), r.say, SpeechPriority::Low, "codriver", expire);
                return;
            }
            if (res == FishResult::Refused) {
                g_fish_off = true;  // bad key, no credit, or a wrong voice/model: don't keep trying
                log_warn("assistant: fish.audio refused the request (%s); check fish_audio_key.txt and [voice] "
                         "fish_voice= / fish_model=. The local voice reads the replies.", why.c_str());
                break;
            }
            log_info("assistant: fish.audio didn't answer (%s)%s", why.c_str(),
                     attempt == 0 ? "; trying again" : "; the local voice reads it");
        }
    }
    speech_say(r.say, SpeechPriority::Low, "codriver", expire, true);
}

static bool pick(Pending& out)
{
    if (g_report.empty()) return false;  // wait for the first situation report
    const ULONGLONG now = GetTickCount64();
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const Pending& p) { return now - p.at > lifetime(p.m.kind); }),
                    g_pending.end());
    const ULONGLONG last = g_last_said;
    auto best = g_pending.end();
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (last && now - last < gap_before(it->m.kind)) continue;
        if (best == g_pending.end() || rank(it->m.kind) < rank(best->m.kind)) best = it;
    }
    if (best == g_pending.end()) return false;
    out = std::move(*best);
    g_pending.erase(best);
    return true;
}

static void worker()
{
    struct Done { ~Done() { g_done = true; } } done;  // however we leave
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);     // for the screenshot's JPEG encoder
    struct Com { ~Com() { CoUninitialize(); } } com;
    const std::string key = gemini_load_key(g_dir);
    if (key.empty()) {
        log_info("assistant: no Gemini API key (set GEMINI_API_KEY or put it in gemini_api_key.txt); assistant off");
        return;
    }
    load_files();
    g_debug = GetEnvironmentVariableW(L"ETS2_CITY_OVERLAY_DEBUG", nullptr, 0) > 0;
    g_fish = fish_voice(g_cfg, g_dir);
    std::string voice;
    if (g_cfg.guide_voice_engine == L"google") voice = "Google (" + narrow(g_cfg.guide_voice) + ")";
    else if (g_cfg.voice_engine == L"fish" && g_fish.usable()) voice = "fish.audio (" + g_fish.model + "), then local";
    else voice = "local";
    log_info("assistant: Gemini %s, voice %s, %zu places known, %zu notes, %d of %d requests left today",
             narrow(g_cfg.guide_model).c_str(), voice.c_str(), g_told.size(), g_notes.size(),
             std::max(0, requests_left()), g_cfg.guide_daily_limit);
    // The model, then any fallbacks from the settings (none by default).
    std::vector<Model> models;
    {
        const std::wstring list = g_cfg.guide_model + L"," + g_cfg.guide_fallback_models;
        std::string chain;
        for (size_t pos = 0; pos <= list.size();) {
            size_t end = list.find(L',', pos);
            if (end == std::wstring::npos) end = list.size();
            std::wstring name = list.substr(pos, end - pos);
            name.erase(0, name.find_first_not_of(L" \t"));
            name.erase(name.find_last_not_of(L" \t") + 1);
            bool dup = false;
            for (const auto& x : models) dup = dup || x.name == name;
            if (!name.empty() && !dup) {
                models.push_back({name});
                chain += (chain.empty() ? "" : ", ") + narrow(name);
            }
            pos = end + 1;
        }
        if (models.size() > 1) log_info("assistant: models in order: %s", chain.c_str());
    }
    ULONGLONG last_call = 0;
    bool told_no_picture = false;

    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        Pending job;
        // Nothing ready (none waiting, or waiting for a quiet gap / the first report): sleep - with
        // the lock released - until something new arrives or a second passes.
        if (!pick(job)) {
            g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_quit || g_wake; });
            g_wake = false;
            if (g_quit) break;
            continue;
        }
        const std::string report = g_report, brief = g_brief;
        lock.unlock();
        const MomentInfo& m = job.m;
        const bool by_driver = asked(m.kind);

        // Automatic remarks respect the daily budgets; the driver asking always gets a try.
        const bool blocked = g_blocked_day == quota_day();
        const int left = requests_left();
        if (!by_driver && (blocked || left < 2)) {
            lock.lock();
            continue;
        }
        if (by_driver && blocked) {
            show("Assistant", "The free Gemini limit for today is used up. The assistant is back tomorrow.");
            if (g_cfg.guide_speak)
                speech_say("The assistant has reached today's free limit. It will be available again tomorrow.",
                           SpeechPriority::Low, "codriver", 30);
            lock.lock();
            continue;
        }
        if (by_driver) show(m.kind == Moment::Talk ? "Assistant" : card_title(m, {}), "", "Thinking...");

        // Be gentle with the free tier.
        const ULONGLONG now = GetTickCount64();
        if (last_call && now - last_call < kMinGap && nap(kMinGap - (now - last_call))) return;

        // The front camera: what's on screen is its main source, so every request gets a picture.
        std::vector<char> jpeg;
        if (g_cfg.guide_screenshots) {
            capture_request();
            if (!capture_wait_jpeg(jpeg, 1500) && !told_no_picture) {
                log_info("assistant: couldn't take a screenshot this time");
                told_no_picture = true;
            }
            save_debug(L"codriver_last.jpg", jpeg);
        }
        if (m.kind == Moment::Look && jpeg.empty()) {  // a look with nothing to look at: don't spend a request
            lock.lock();
            continue;
        }


        std::string system = kSystem;
        if (!g_notes.empty()) {
            system += "\nWhat you remember about the driver from earlier drives:";
            for (const auto& n : g_notes) system += "\n- " + n;
        }
        std::string contents;
        for (const auto& ex : g_history)
            contents += "{\"role\":\"user\",\"parts\":[{\"text\":" + json_quote(ex.user) + "}]}," +
                        "{\"role\":\"model\",\"parts\":[{\"text\":" + json_quote("{\"say\":" + json_quote(ex.model) + "}") +
                        "}]},";
        // The task first, then the report it refers to (small models follow it better that way).
        const std::string task = instruction(m, !jpeg.empty());
        std::string parts = "{\"text\":" + json_quote("Task: " + task + "\n\nSituation report:\n" + report) + "}";
        if (!jpeg.empty())
            parts += ",{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"" + base64_encode(jpeg.data(), jpeg.size()) + "\"}}";
        if (!m.audio.empty())
            parts += ",{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"" + base64_encode(m.audio.data(), m.audio.size()) + "\"}}";
        contents += "{\"role\":\"user\",\"parts\":[" + parts + "]}";
        if (g_debug) {  // what the model is told this time, for checking
            std::string dump = "--- " + std::string(name_of(m.kind)) + "\n";
            for (const auto& ex : g_history) dump += "user: " + ex.user + "\nmodel: " + ex.model + "\n";
            dump += "now:\nTask: " + task + "\n\nSituation report:\n" + report + "\n\n";
            if (FILE* f = _wfopen((g_dir + L"\\codriver_requests.txt").c_str(), L"ab")) {
                fputs(dump.c_str(), f);
                fclose(f);
            }
        }

        std::vector<Model*> order;  // the models to try, in order
        for (auto& x : models) order.push_back(&x);
        const bool with_audio = !m.audio.empty();
        auto usable = [&](const Model* x, bool now_too) {
            return !x->broken && !(with_audio && x->no_audio) && x->spent_day != quota_day() &&
                   (!now_too || GetTickCount64() >= x->busy_until);
        };

        Reply reply;
        std::string why;
        bool got = false, off = false;
        int waits = 0;  // naps for network trouble or when every model is busy
        for (int attempt = 0; attempt < 12 && !got; ++attempt) {
            Model* md = nullptr;
            for (Model* x : order)
                if (usable(x, true)) { md = x; break; }
            if (!md) {
                ULONGLONG soonest = 0;
                for (const Model* x : order)
                    if (usable(x, false) && (!soonest || x->busy_until < soonest)) soonest = x->busy_until;
                if (!soonest) {  // every model is used up (or can't be used) for today
                    g_blocked_day = quota_day();
                    why = "every model's free daily limit is used up";
                    log_info("assistant: %s; back tomorrow", why.c_str());
                    break;
                }
                // All busy for a moment: the driver asking is worth a short wait, a remark isn't.
                const ULONGLONG wait = soonest - std::min(soonest, GetTickCount64());
                if (!by_driver || waits++ >= 1 || wait > 30000) { why = "Gemini is busy"; break; }
                if (nap(wait + 200)) return;
                continue;
            }
            const std::string name = narrow(md->name);
            auto next_name = [&]() -> std::string {
                for (const Model* x : order)
                    if (x != md && usable(x, false)) return narrow(x->name);
                return "nothing (until tomorrow)";
            };
            const std::string body =
                std::string("{\"systemInstruction\":{\"parts\":[{\"text\":") + json_quote(system) + "}]}," +
                "\"contents\":[" + contents + "]," +
                "\"generationConfig\":{\"maxOutputTokens\":2048,\"temperature\":1.0," +
                "\"responseMimeType\":\"application/json\",\"responseSchema\":" + kSchema +
                // A little more thought reads the picture more carefully (it would sometimes mix up
                // two signs); the driver's own questions stay quick.
                (md->thinking ? std::string(",\"thinkingConfig\":{\"thinkingLevel\":\"") +
                                    (m.kind == Moment::Talk ? "low" : "medium") + "\"}"
                              : std::string()) +
                "}}";
            last_call = GetTickCount64();
            count_request();
            const GeminiReply r = gemini_post(L"/v1beta/models/" + md->name + L":generateContent", body, key);
            if (r.ok()) {
                got = parse_reply(r.json, reply, why);
                if (got)
                    log_info("assistant: %s answer from %s in %llu ms", name_of(m.kind), name.c_str(),
                             GetTickCount64() - last_call);
                break;
            }
            if (r.status == 400 && md->thinking && r.message.find("hinking") != std::string::npos) {
                md->thinking = false;  // this model doesn't take the setting: ask again without it
                continue;
            }
            if (r.key_rejected()) {
                log_warn("assistant: Gemini rejected the API key (%d); assistant off until the key is fixed", r.status);
                show("Assistant", "Google rejected the Gemini API key. Check gemini_api_key.txt.");
                off = true;
                break;
            }
            if (r.status == 429 && r.daily_limit()) {
                md->spent_day = quota_day();
                const std::string limit = r.quota_limit();
                log_info("assistant: %s's free daily limit%s%s%s is used up; switching to %s", name.c_str(),
                         limit.empty() ? "" : " (", limit.c_str(), limit.empty() ? "" : " requests)",
                         next_name().c_str());
                continue;
            }
            if (r.status == 429 || r.status == 503) {  // per-minute limit or "high demand": another one for now
                md->busy_until = GetTickCount64() + (ULONGLONG)std::max(20, r.retry_seconds()) * 1000;
                log_info("assistant: %s is busy (%d); trying %s", name.c_str(), r.status, next_name().c_str());
                continue;
            }
            if (r.status == 404 || r.status == 400) {
                // Gone or renamed, or it can't take this request: the next one.
                if (r.status == 400 && with_audio) md->no_audio = true;
                else md->broken = true;
                log_warn("assistant: %s can't be used%s (%d: %s); switching to %s", name.c_str(),
                         md->no_audio && !md->broken ? " for voice questions" : "", r.status,
                         r.message.substr(0, 160).c_str(), next_name().c_str());
                continue;
            }
            // No connection, or a server hiccup.
            why = r.status < 0 ? "no connection" : "server error " + std::to_string(r.status);
            if (waits++ >= 2) break;
            log_info("assistant: Gemini %s after %llu ms; trying again", why.c_str(), GetTickCount64() - last_call);
            if (nap(5000)) return;
        }
        if (off) return;

        if (!got) {
            log_info("assistant: no answer (%s) - %s", name_of(m.kind), why.c_str());
            if (by_driver) show("Assistant", "No answer right now (" + why + ").");
        } else {
            if (m.audio.empty()) reply.heard.clear();  // nothing was said: don't let it make words up
            if (!reply.heard.empty()) log_info("assistant heard: \"%s\"", reply.heard.c_str());
            g_history.push_back({history_line(m, brief, reply.heard), reply.say});
            while (g_history.size() > kHistory) g_history.pop_front();
            if (!reply.remember.empty()) remember_note(reply.remember);
            if (reply.say.empty()) {
                log_info("assistant: nothing to say (%s)", name_of(m.kind));
                if (by_driver) show(card_title(m, reply), "...");
            } else {
                if (m.kind == Moment::City || m.kind == Moment::AskHere) remember_place(m.place_id, reply.say);
                show(card_title(m, reply), reply.say);
                speak(key, reply, m);
                g_last_said = GetTickCount64();
            }
        }
        lock.lock();
    }
}

// --- API ------------------------------------------------------------------------------------

void codriver_start(const Config& cfg, const std::wstring& dir)
{
    if (g_thread.joinable()) return;
    g_cfg = cfg;
    g_dir = dir;
    g_quit = false;
    g_done = false;
    g_thread = std::thread(worker);
}

void codriver_stop()
{
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quit = true;
    }
    g_cv.notify_all();
    for (int i = 0; i < 300 && !g_done; ++i) Sleep(10);
    if (g_done) {
        g_thread.join();
        return;
    }
    // A request is still on the wire (network timeouts can be long). Don't hold the game's exit:
    // keep this DLL loaded until the process ends so the thread can finish safely.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&codriver_stop), &self);
    g_thread.detach();
}

void codriver_set_situation(const std::string& report, const std::string& brief)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_report = report;
    g_brief = brief;
}

void codriver_moment(MomentInfo m)
{
    if (!g_thread.joinable() || g_done) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // The newest of a kind replaces an older one still waiting (the driver talking queues up).
        if (m.kind != Moment::Talk)
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [&](const Pending& p) { return p.m.kind == m.kind; }),
                            g_pending.end());
        if (g_pending.size() < 8) g_pending.push_back({std::move(m), GetTickCount64()});
        g_wake = true;
    }
    g_cv.notify_all();
}

double codriver_quiet_seconds()
{
    const ULONGLONG last = g_last_said;
    return last ? (GetTickCount64() - last) / 1000.0 : 1e9;
}
