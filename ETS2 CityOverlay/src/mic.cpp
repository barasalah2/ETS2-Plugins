#include "mic.h"
#include "log.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

static const int      kRate = 16000;
static const int      kMaxSeconds = 20;
static const ULONGLONG kTapMs = 350;  // shorter than this is a tap

static std::thread             g_thread;
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static bool                    g_quit = false, g_down = false;
static int                     g_presses = 0;  // key-down edges not handled yet (a quick tap may be over before we look)
static std::atomic<bool>       g_listening{false};
static bool                    g_record = true;
static std::deque<MicResult>   g_results;

// Test hook: ETS2_CITY_OVERLAY_FAKE_MIC=<file.wav> is "recorded" instead of the microphone.
static std::vector<char> fake_recording()
{
    wchar_t path[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"ETS2_CITY_OVERLAY_FAKE_MIC", path, MAX_PATH)) return {};
    std::vector<char> data;
    if (FILE* f = _wfopen(path, L"rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    return data;
}

static std::vector<char> make_wav(const char* pcm, size_t bytes)
{
    std::vector<char> wav(44 + bytes);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, (uint32_t)(36 + bytes));
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);  // PCM
    put16(22, 1);  // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, (uint32_t)bytes);
    if (bytes) memcpy(&wav[44], pcm, bytes);
    return wav;
}

static void push(MicResult r)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_results.size() < 4) g_results.push_back(std::move(r));
}

static void worker()
{
    bool warned = false;
    std::vector<char> buffer((size_t)kRate * 2 * kMaxSeconds);
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        g_cv.wait(lock, [] { return g_quit || g_presses > 0; });
        if (g_quit) break;
        g_presses = 0;
        lock.unlock();

        const ULONGLONG t0 = GetTickCount64();
        std::vector<char> fake = g_record ? fake_recording() : std::vector<char>();
        HWAVEIN in = nullptr;
        WAVEHDR hdr = {};
        bool recording = false;
        if (!g_record) {
            // taps only
        } else if (fake.empty()) {
            WAVEFORMATEX fmt = {};
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = 1;
            fmt.nSamplesPerSec = kRate;
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = 2;
            fmt.nAvgBytesPerSec = kRate * 2;
            const MMRESULT res = waveInOpen(&in, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
            if (res == MMSYSERR_NOERROR) {
                hdr.lpData = buffer.data();
                hdr.dwBufferLength = (DWORD)buffer.size();
                recording = waveInPrepareHeader(in, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR &&
                            waveInAddBuffer(in, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR &&
                            waveInStart(in) == MMSYSERR_NOERROR;
            }
            if (!recording && !warned) {
                log_warn("co-driver: no microphone could be opened (error %u); the key only asks about where you are",
                         (unsigned)res);
                warned = true;
            }
        } else {
            recording = true;
        }
        g_listening = recording;

        // Until the key is let go (or the buffer is full).
        lock.lock();
        g_cv.wait_for(lock, std::chrono::seconds(kMaxSeconds), [] { return g_quit || !g_down; });
        const bool quit = g_quit;
        lock.unlock();
        const ULONGLONG held = GetTickCount64() - t0;
        g_listening = false;

        MicResult r;
        if (in) {
            waveInReset(in);  // hands back the buffer with what was recorded so far
            waveInUnprepareHeader(in, &hdr, sizeof(hdr));
            waveInClose(in);
            if (held >= kTapMs && hdr.dwBytesRecorded > (DWORD)kRate)  // at least half a second of audio
                r.wav = make_wav(buffer.data(), hdr.dwBytesRecorded);
        } else if (recording && held >= kTapMs) {
            r.wav = std::move(fake);
        }
        r.tap = r.wav.empty();
        if (!quit) {
            if (r.tap) log_info("co-driver: key tapped");
            else log_info("co-driver: recorded %.1f s of speech", (r.wav.size() - 44) / (2.0 * kRate));
            push(std::move(r));
        }
        lock.lock();
        if (g_quit) break;
    }
}

void mic_start(bool record)
{
    if (g_thread.joinable()) return;
    g_record = record;
    g_quit = false;
    g_down = false;
    g_thread = std::thread(worker);
}

void mic_stop()
{
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quit = true;
    }
    g_cv.notify_all();
    g_thread.join();
}

void mic_hold(bool down)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (down && !g_down) ++g_presses;
        g_down = down;
    }
    g_cv.notify_all();
}

bool mic_listening() { return g_listening; }

bool mic_take(MicResult& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_results.empty()) return false;
    out = std::move(g_results.front());
    g_results.pop_front();
    return true;
}
