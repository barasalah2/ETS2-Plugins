#pragma once

// Minimal JSON: parse a document into a tree, and quote strings for writing. Enough for API
// responses (objects, arrays, strings with \u escapes, numbers, true/false/null).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json& operator[](const std::string& key) const
    {
        static const Json none;
        auto it = obj.find(key);
        return it == obj.end() ? none : it->second;
    }
    const Json& operator[](size_t i) const
    {
        static const Json none;
        return i < arr.size() ? arr[i] : none;
    }
};

namespace json_detail {

inline void skip_ws(const char*& p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

inline void put_utf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

inline bool parse_hex4(const char*& p, const char* end, unsigned& v)
{
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i, ++p) {
        const char c = *p;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return false;
    }
    return true;
}

inline bool parse_string(const char*& p, const char* end, std::string& out)
{
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
        if (*p != '\\') {
            out += *p++;
            continue;
        }
        if (++p >= end) return false;
        const char e = *p++;
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(p, end, cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {  // surrogate pair
                    p += 2;
                    unsigned lo;
                    if (!parse_hex4(p, end, lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    if (p >= end) return false;
    ++p;  // closing quote
    return true;
}

inline bool parse_value(const char*& p, const char* end, Json& v, int depth)
{
    if (depth > 64) return false;
    skip_ws(p, end);
    if (p >= end) return false;
    const char c = *p;
    if (c == '{') {
        v.type = Json::Object;
        ++p;
        skip_ws(p, end);
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            skip_ws(p, end);
            std::string key;
            if (!parse_string(p, end, key)) return false;
            skip_ws(p, end);
            if (p >= end || *p != ':') return false;
            ++p;
            if (!parse_value(p, end, v.obj[key], depth + 1)) return false;
            skip_ws(p, end);
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return false;
        }
    }
    if (c == '[') {
        v.type = Json::Array;
        ++p;
        skip_ws(p, end);
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            v.arr.emplace_back();
            if (!parse_value(p, end, v.arr.back(), depth + 1)) return false;
            skip_ws(p, end);
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return false;
        }
    }
    if (c == '"') {
        v.type = Json::String;
        return parse_string(p, end, v.str);
    }
    if (end - p >= 4 && std::string(p, 4) == "true") { v.type = Json::Bool; v.b = true; p += 4; return true; }
    if (end - p >= 5 && std::string(p, 5) == "false") { v.type = Json::Bool; p += 5; return true; }
    if (end - p >= 4 && std::string(p, 4) == "null") { v.type = Json::Null; p += 4; return true; }
    const std::string tail(p, std::min<size_t>(end - p, 64));
    char* num_end = nullptr;
    v.num = strtod(tail.c_str(), &num_end);
    if (num_end == tail.c_str()) return false;
    v.type = Json::Number;
    p += num_end - tail.c_str();
    return true;
}

}  // namespace json_detail

// Returns false on malformed input.
inline bool json_parse(const std::string& text, Json& out)
{
    const char* p = text.data();
    const char* end = p + text.size();
    out = Json{};
    return json_detail::parse_value(p, end, out, 0);
}

// UTF-8 text -> a quoted JSON string.
inline std::string json_quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}
