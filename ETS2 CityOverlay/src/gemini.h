#pragma once

#include "json.h"

#include <string>
#include <vector>

// Google Gemini REST calls (free tier) over WinHTTP, for the co-driver. Blocking: call from a
// worker thread.
//
// The API key comes from the GEMINI_API_KEY environment variable, or the first line of
// gemini_api_key.txt in the plugin's data folder. It is never logged.
std::string gemini_load_key(const std::wstring& dir);

struct GeminiReply
{
    int         status = -1;  // HTTP status, -1 = no connection (network, DNS, TLS...)
    bool        parsed = false;
    Json        json;
    std::string message;      // the error message, when there is one

    bool ok() const { return status == 200 && parsed; }
    bool key_rejected() const;  // bad or missing key: nothing will work until it's fixed
    bool daily_limit() const;   // 429 because today's free quota is used up (not the per-minute one)
    int  retry_seconds() const; // the wait a 429 asks for (0 = not given)
    std::string quota_limit() const;  // the limit a 429 names, e.g. "20" ("" = not given)
};

// POST https://generativelanguage.googleapis.com<path> with a JSON body.
GeminiReply gemini_post(const std::wstring& path, const std::string& body, const std::string& key);

// Plain HTTPS POST (also used for the fish.audio voice). `headers` are "Name: value\r\n" lines.
// Returns the HTTP status, or -1 if the request couldn't be made (or took longer than timeout_ms).
int https_post(const wchar_t* host, const std::wstring& path, const std::string& headers, const std::string& body,
               std::string& response, int timeout_ms = 45000);

// First line of <dir>\<file>, or the environment variable when it's set. Keys are never logged.
std::string read_secret(const std::wstring& dir, const wchar_t* file, const char* env);

std::string base64_encode(const void* data, size_t size);
bool        base64_decode(const std::string& text, std::vector<char>& out);
