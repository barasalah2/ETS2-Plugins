#pragma once

#include <string>
#include <vector>

// Minimal UTF-8 CSV: comma separated, optional "quoted" fields with "" escapes.
std::vector<std::vector<std::string>> read_csv(const std::wstring& path);
std::string csv_escape(const std::string& s);
