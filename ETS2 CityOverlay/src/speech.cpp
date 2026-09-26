#include "speech.h"
#include "log.h"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// --- shared state ---------------------------------------------------------------------------

struct Message
{
    std::string text;
    SpeechPriority priority;
    std::string key;
    ULONGLONG expires;
    std::shared_ptr<std::vector<char>> wav;  // ready-made audio instead of text to synthesize
};

static std::thread             g_thread;
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static std::vector<Message>    g_queue;
static bool                    g_quit = false, g_paused = false;
static std::atomic<int>        g_playing{-1};  // priority of the message playing now, -1 = none
static std::atomic<bool>       g_available{false};
static Config                  g_cfg;
static std::wstring            g_dir;

static std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool env_set(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; }

// --- Piper helper process -------------------------------------------------------------------

// Piper reads one line of text per utterance on stdin, writes a WAV into --output_dir and prints
// its path on stdout. Keeping it running keeps the voice model loaded between messages.
class Piper
{
public:
    bool start(const std::wstring& exe, const std::wstring& model, const std::wstring& out_dir, double speed)
    {
        out_dir_ = out_dir;
        CreateDirectoryW(out_dir.c_str(), nullptr);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE in_r = nullptr, out_w = nullptr;
        if (!CreatePipe(&in_r, &in_w_, &sa, 0) || !CreatePipe(&out_r_, &out_w, &sa, 0)) return false;
        SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
        HANDLE err = CreateFileW((out_dir + L"\\piper.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        // length_scale > 1 speaks slower.
        wchar_t scale[32];
        swprintf(scale, 32, L"%.2f", 1.0 / std::clamp(speed, 0.5, 2.0));
        std::wstring cmd = L"\"" + exe + L"\" --model \"" + model + L"\" --output_dir \"" + out_dir +
                           L"\" --length_scale " + scale + L" --sentence_silence 0.25";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = err != INVALID_HANDLE_VALUE ? err : out_w;
        PROCESS_INFORMATION pi = {};
        std::wstring workdir = exe.substr(0, exe.find_last_of(L"\\/"));
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, workdir.c_str(),
                                       &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
        if (!ok) return false;
        proc_ = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
    }

    // Returns the WAV file Piper wrote, or "" on failure.
    std::wstring synth(const std::string& text)
    {
        if (!proc_) return {};
        std::string line = text;
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        line += "\n";
        DWORD written = 0;
        if (!WriteFile(in_w_, line.data(), (DWORD)line.size(), &written, nullptr)) return {};

        std::string out;
        const ULONGLONG deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_r_, nullptr, 0, nullptr, &avail, nullptr)) return {};
            if (avail) {
                char buf[512];
                DWORD got = 0;
                if (!ReadFile(out_r_, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr)) return {};
                out.append(buf, got);
                const size_t nl = out.find('\n');
                if (nl != std::string::npos) {
                    std::string path = out.substr(0, nl);
                    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
                    return widen(path);
                }
                continue;
            }
            if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) return {};  // Piper exited
            Sleep(10);
        }
        return {};
    }

    bool running() const { return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT; }

    void stop()
    {
        if (in_w_) { CloseHandle(in_w_); in_w_ = nullptr; }  // Piper exits at end of input
        if (proc_) {
            if (WaitForSingleObject(proc_, 2000) != WAIT_OBJECT_0) TerminateProcess(proc_, 0);
            CloseHandle(proc_);
            proc_ = nullptr;
        }
        if (out_r_) { CloseHandle(out_r_); out_r_ = nullptr; }
    }

private:
    HANDLE proc_ = nullptr, in_w_ = nullptr, out_r_ = nullptr;
    std::wstring out_dir_;
};

// --- audio ------------------------------------------------------------------------------------

static std::vector<char> read_file(const std::wstring& path)
{
    std::vector<char> data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 44 && size.QuadPart < 64 * 1024 * 1024) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    return data;
}

// Scales 16-bit PCM samples in a WAV image in place (PlaySound has no volume control).
static void apply_volume(std::vector<char>& wav, int volume)
{
    if (volume >= 100 || wav.size() < 12 || memcmp(wav.data(), "RIFF", 4) != 0) return;
    size_t pos = 12;
    int bits = 16;
    while (pos + 8 <= wav.size()) {
        uint32_t len;
        memcpy(&len, &wav[pos + 4], 4);
        if (memcmp(&wav[pos], "fmt ", 4) == 0 && pos + 8 + 16 <= wav.size()) {
            uint16_t b;
            memcpy(&b, &wav[pos + 8 + 14], 2);
            bits = b;
        } else if (memcmp(&wav[pos], "data", 4) == 0 && bits == 16) {
            const size_t end = std::min(wav.size(), pos + 8 + (size_t)len);
            const float k = std::max(0, volume) / 100.0f;
            for (size_t i = pos + 8; i + 1 < end; i += 2) {
                int16_t v;
                memcpy(&v, &wav[i], 2);
                v = (int16_t)std::clamp((int)(v * k), -32768, 32767);
                memcpy(&wav[i], &v, 2);
            }
            return;
        }
        pos += 8 + len + (len & 1);
    }
}

// --- worker -----------------------------------------------------------------------------------

static bool pop_next(Message& out)
{
    const ULONGLONG now = GetTickCount64();
    g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(), [&](const Message& m) { return m.expires < now; }),
                  g_queue.end());
    if (g_paused || g_queue.empty()) return false;
    auto best = g_queue.begin();
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        if ((int)it->priority > (int)best->priority) best = it;  // FIFO within a priority
    out = std::move(*best);
    g_queue.erase(best);
    return true;
}

static void worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mute = env_set(L"ETS2_CITY_OVERLAY_MUTE");  // tests: synthesize but don't play

    // Voice files: <plugin data>\voice\piper\piper.exe and <plugin data>\voice\<model>.onnx
    wchar_t override_dir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"ETS2_CITY_OVERLAY_VOICE_DIR", override_dir, MAX_PATH);
    const std::wstring voice_dir = override_dir[0] ? std::wstring(override_dir) : g_dir + L"\\voice";
    const std::wstring exe = voice_dir + L"\\piper\\piper.exe";
    const std::wstring model = voice_dir + L"\\" + g_cfg.voice_model + L".onnx";

    Piper piper;
    ISpVoice* sapi = nullptr;
    auto start_sapi = [&] {
        if (sapi) return;
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sapi)))) {
            sapi = nullptr;
            return;
        }
        sapi->SetVolume((USHORT)std::clamp(g_cfg.voice_volume, 0, 100));
        sapi->SetRate(std::clamp(g_cfg.voice_rate, -10, 10));
    };
    bool use_piper = g_cfg.voice_engine != L"windows";
    if (use_piper) {
        const bool have = GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES &&
                          GetFileAttributesW(model.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (have && piper.start(exe, model, voice_dir + L"\\cache", 1.0 + g_cfg.voice_rate * 0.05)) {
            log_info("speech: natural voice '%s' (Piper)", narrow(g_cfg.voice_model).c_str());
        } else {
            log_warn("speech: Piper voice '%s' not found in %s; using the Windows voice",
                     narrow(g_cfg.voice_model).c_str(), narrow(voice_dir).c_str());
            use_piper = false;
        }
    }
    if (!use_piper) start_sapi();
    g_available = use_piper || sapi;

    std::unordered_map<std::string, std::vector<char>> cache;  // repeated phrases, volume applied
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        Message msg;
        g_cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return g_quit || (!g_paused && !g_queue.empty()); });
        if (g_quit) break;
        if (!pop_next(msg)) continue;
        lock.unlock();

        g_playing = (int)msg.priority;
        const ULONGLONG t0 = GetTickCount64();
        bool said = false;
        if (msg.wav) {
            apply_volume(*msg.wav, g_cfg.voice_volume);
            log_info("speech (co-driver voice): \"%s\"", msg.text.c_str());
            if (!mute) PlaySoundW((LPCWSTR)msg.wav->data(), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
            said = true;
        }
        if (!said && use_piper) {
            auto hit = cache.find(msg.text);
            std::vector<char> wav;
            if (hit != cache.end()) {
                wav = hit->second;
            } else {
                const std::wstring path = piper.synth(msg.text);
                if (!path.empty()) {
                    wav = read_file(path);
                    DeleteFileW(path.c_str());
                    apply_volume(wav, g_cfg.voice_volume);
                    if (cache.size() > 48) cache.clear();
                    if (!wav.empty()) cache[msg.text] = wav;
                } else if (!piper.running()) {
                    log_warn("speech: Piper stopped; switching to the Windows voice");
                    piper.stop();
                    use_piper = false;
                    start_sapi();
                }
            }
            if (!wav.empty()) {
                const ULONGLONG synth_ms = GetTickCount64() - t0;
                log_info("speech: \"%s\" (%llu ms to prepare)", msg.text.c_str(), synth_ms);
                if (!mute) PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
                said = true;
            }
        }
        if (!said && sapi) {
            log_info("speech (Windows voice): \"%s\"", msg.text.c_str());
            if (!mute) sapi->Speak(widen(msg.text).c_str(), SPF_IS_NOT_XML, nullptr);  // blocking, like PlaySound
        }
        g_playing = -1;
        lock.lock();
    }
    lock.unlock();
    PlaySoundW(nullptr, nullptr, 0);
    piper.stop();
    if (sapi) sapi->Release();
    CoUninitialize();
}

// --- API ------------------------------------------------------------------------------------

void speech_start(const Config& cfg, const std::wstring& dir)
{
    if (g_thread.joinable()) return;
    g_cfg = cfg;
    g_dir = dir;
    g_quit = false;
    g_thread = std::thread(worker);
}

void speech_stop()
{
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quit = true;
        g_queue.clear();
    }
    PlaySoundW(nullptr, nullptr, 0);  // cut off anything playing so the worker can finish
    g_cv.notify_one();
    g_thread.join();
    g_available = false;
}

static void enqueue(Message m);

void speech_say(const std::string& text, SpeechPriority priority, const std::string& key, int expire_s)
{
    if (!g_thread.joinable() || text.empty()) return;
    enqueue({text, priority, key, GetTickCount64() + (ULONGLONG)std::max(1, expire_s) * 1000, nullptr});
}

// Streamed WAVs (fish.audio) carry placeholder sizes (~4 GB); PlaySound needs the real ones.
static void fix_wav_sizes(std::vector<char>& wav)
{
    if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0) return;
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

void speech_play(std::vector<char> wav, const std::string& text, SpeechPriority priority, const std::string& key,
                 int expire_s)
{
    if (!g_thread.joinable() || wav.size() <= 44) return;
    fix_wav_sizes(wav);
    enqueue({text, priority, key, GetTickCount64() + (ULONGLONG)std::max(1, expire_s) * 1000,
             std::make_shared<std::vector<char>>(std::move(wav))});
}

static void enqueue(Message m)
{
    const SpeechPriority priority = m.priority;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bool replaced = false;
        if (!m.key.empty())
            for (auto& q : g_queue)
                if (q.key == m.key) { q = m; replaced = true; break; }
        if (!replaced) {
            if (g_queue.size() >= 16) g_queue.erase(g_queue.begin());
            g_queue.push_back(std::move(m));
        }
    }
    // An alert cuts off the tour guide mid-sentence.
    if (priority == SpeechPriority::High && g_playing == (int)SpeechPriority::Low) PlaySoundW(nullptr, nullptr, 0);
    g_cv.notify_one();
}

void speech_set_paused(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_paused = paused;
    }
    if (paused) PlaySoundW(nullptr, nullptr, 0);  // stop talking when the menu opens
    g_cv.notify_one();
}

bool speech_available() { return g_available; }
