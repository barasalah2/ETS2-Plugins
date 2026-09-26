#include "csv.h"

#include <cstdio>

// Minimal CSV reader: comma separated, optional "quoted" fields with "" escapes, UTF-8.
std::vector<std::vector<std::string>> read_csv(const std::wstring& path)
{
    std::vector<std::vector<std::string>> rows;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return rows;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF) data.erase(0, 3);  // BOM

    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < data.size(); ++i) {
        char c = data[i];
        if (quoted) {
            if (c == '"' && i + 1 < data.size() && data[i + 1] == '"') { field += '"'; ++i; }
            else if (c == '"') quoted = false;
            else field += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row.push_back(std::move(field)); field.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n') ++i;
            row.push_back(std::move(field)); field.clear();
            if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row));
            row.clear();
        } else {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) { row.push_back(std::move(field)); rows.push_back(std::move(row)); }
    return rows;
}

std::string csv_escape(const std::string& s)
{
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}
