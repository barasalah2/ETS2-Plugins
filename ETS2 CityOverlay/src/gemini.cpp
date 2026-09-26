#include "gemini.h"

#include <windows.h>
#include <winhttp.h>

static std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string trim(std::string s)
{
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    const size_t e = s.find_last_not_of(ws);
    s.erase(e == std::string::npos ? 0 : e + 1);
    return s;
}

std::string gemini_load_key(const std::wstring& dir) { return read_secret(dir, L"gemini_api_key.txt", "GEMINI_API_KEY"); }

std::string read_secret(const std::wstring& dir, const wchar_t* file, const char* env)
{
    char buf[512];
    DWORD n = GetEnvironmentVariableA(env, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return trim(buf);
    FILE* f = _wfopen((dir + L"\\" + file).c_str(), L"rb");
    if (!f) return {};
    std::string line;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') line += (char)c;
    fclose(f);
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // BOM
    return trim(line);
}

// Errors come as {"error":{...}} or, for some, [{"error":{...}}].
static const Json& error_of(const Json& j) { return j.type == Json::Array ? j[0]["error"] : j["error"]; }

bool GeminiReply::key_rejected() const
{
    if (status == 401 || status == 403) return true;
    if (status != 400) return false;
    for (const Json& d : error_of(json)["details"].arr)
        if (d["reason"].str == "API_KEY_INVALID") return true;
    return message.find("API key") != std::string::npos;
}

bool GeminiReply::daily_limit() const
{
    if (status != 429) return false;
    for (const Json& d : error_of(json)["details"].arr)
        for (const Json& v : d["violations"].arr)
            if (v["quotaId"].str.find("PerDay") != std::string::npos) return true;
    return message.find("per day") != std::string::npos || message.find("daily") != std::string::npos;
}

std::string GeminiReply::quota_limit() const
{
    for (const Json& d : error_of(json)["details"].arr)
        for (const Json& v : d["violations"].arr) {
            if (!v["quotaValue"].str.empty()) return v["quotaValue"].str;
            if (v["quotaValue"].type == Json::Number) return std::to_string((long long)v["quotaValue"].num);
        }
    return {};
}

int GeminiReply::retry_seconds() const
{
    for (const Json& d : error_of(json)["details"].arr)
        if (!d["retryDelay"].str.empty()) return std::max(1, atoi(d["retryDelay"].str.c_str()));  // "37s"
    return 0;
}

int https_post(const wchar_t* host, const std::wstring& path, const std::string& headers, const std::string& body,
               std::string& response)
{
    response.clear();
    int status = -1;
    HINTERNET session = WinHttpOpen(L"ETS2CityOverlay/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return status;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 45000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request) {
        const std::wstring h = widen(headers);
        if (WinHttpSendRequest(request, h.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
            DWORD avail = 0;
            // Audio replies are up to a few MB.
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && response.size() < (16u << 20)) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &got) || got == 0) break;
                response.append(chunk.data(), got);
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

GeminiReply gemini_post(const std::wstring& path, const std::string& body, const std::string& key)
{
    GeminiReply r;
    std::string response;
    r.status = https_post(L"generativelanguage.googleapis.com", path,
                          "Content-Type: application/json\r\nx-goog-api-key: " + key + "\r\n", body, response);
    r.parsed = json_parse(response, r.json);
    if (r.parsed) r.message = error_of(r.json)["message"].str;
    return r;
}

// --- base64 -------------------------------------------------------------------------------------

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const void* data, size_t size)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned v = (unsigned)p[i] << 16 | (i + 1 < size ? (unsigned)p[i + 1] << 8 : 0) |
                           (i + 2 < size ? (unsigned)p[i + 2] : 0);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < size ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < size ? kB64[v & 63] : '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<char>& out)
{
    static signed char table[256];
    static bool init = [] {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 64; ++i) table[(unsigned char)kB64[i]] = (signed char)i;
        table[(unsigned char)'-'] = 62;  // URL-safe variant
        table[(unsigned char)'_'] = 63;
        return true;
    }();
    (void)init;
    out.clear();
    out.reserve(text.size() / 4 * 3);
    unsigned v = 0;
    int bits = 0;
    for (unsigned char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int d = table[c];
        if (d < 0) return false;
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((v >> bits) & 0xFF));
        }
    }
    return !out.empty();
}
