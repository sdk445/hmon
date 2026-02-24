#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace linux_utils {
namespace fs = std::filesystem;

inline bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

inline bool endsWith(const std::string& text, const std::string& suffix) {
  if (suffix.size() > text.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin());
}

inline std::string trim(const std::string& input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}

inline std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline std::optional<std::string> readFirstLine(const fs::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::string line;
  std::getline(file, line);
  return trim(line);
}

inline std::optional<long long> readLongLong(const fs::path& path) {
  const auto value = readFirstLine(path);
  if (!value) {
    return std::nullopt;
  }
  try {
    return std::stoll(*value);
  } catch (...) {
    return std::nullopt;
  }
}

inline std::optional<double> normalizePercent(long long raw_value) {
  if (raw_value < 0 || raw_value > 100) {
    return std::nullopt;
  }
  return static_cast<double>(raw_value);
}

inline std::optional<double> normalizeTemperatureC(long long raw_value) {
  double celsius = static_cast<double>(raw_value);
  if (std::abs(celsius) > 1000.0) {
    celsius /= 1000.0;
  }
  if (celsius < 0.0 || celsius > 150.0) {
    return std::nullopt;
  }
  return celsius;
}

inline std::optional<double> microWattsToWatts(long long micro_watts) {
  if (micro_watts <= 0) {
    return std::nullopt;
  }
  const double watts = static_cast<double>(micro_watts) / 1000000.0;
  if (watts <= 0.0 || watts > 2000.0) {
    return std::nullopt;
  }
  return watts;
}

inline std::optional<double> readHwmonPowerWatts(const fs::path& hwmon_dir) {
  static const std::array<const char*, 6> candidates = {"power1_average", "power1_input", "power2_average",
                                                         "power2_input", "power_average", "power_input"};
  for (const auto* name : candidates) {
    const auto raw = readLongLong(hwmon_dir / name);
    if (raw) {
      const auto watts = microWattsToWatts(*raw);
      if (watts) {
        return watts;
      }
    }
  }
  return std::nullopt;
}

inline std::optional<long long> readFirstExistingLongLong(const std::vector<fs::path>& candidates) {
  for (const auto& path : candidates) {
    const auto value = readLongLong(path);
    if (value) {
      return value;
    }
  }
  return std::nullopt;
}

inline std::optional<double> parseOptionalDouble(const std::string& input) {
  const std::string cleaned = trim(input);
  if (cleaned.empty()) {
    return std::nullopt;
  }
  const std::string lower = toLower(cleaned);
  if (lower == "n/a" || lower == "na" || lower == "[not supported]") {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const double value = std::stod(cleaned, &consumed);
    if (consumed == 0) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

inline std::optional<double> extractFirstNumber(const std::string& input) {
  const std::regex pattern(R"(([0-9]+(?:\.[0-9]+)?))");
  std::smatch match;
  if (!std::regex_search(input, match, pattern)) {
    return std::nullopt;
  }
  try {
    return std::stod(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

inline std::optional<double> extractWattsFromText(const std::string& input) {
  const std::regex watts_pattern(R"(([0-9]+(?:\.[0-9]+)?)\s*([mM]?)\s*[Ww]\b)");
  std::smatch match;
  if (!std::regex_search(input, match, watts_pattern)) {
    return std::nullopt;
  }
  try {
    double watts = std::stod(match[1].str());
    if (match[2].matched && !match[2].str().empty()) {
      watts /= 1000.0;
    }
    if (watts <= 0.0 || watts > 2000.0) {
      return std::nullopt;
    }
    return watts;
  } catch (...) {
    return std::nullopt;
  }
}

inline std::vector<std::string> splitByChar(const std::string& line, char delimiter) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, delimiter)) {
    out.push_back(trim(item));
  }
  return out;
}

inline std::vector<fs::path> listDirEntries(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::exists(dir)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

inline std::string runCommand(const std::string& command) {
  std::array<char, 256> buffer{};
  std::string output;

  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return output;
  }

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  pclose(pipe);

  return output;
}

}  // namespace linux_utils
