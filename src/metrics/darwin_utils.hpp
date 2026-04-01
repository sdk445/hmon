#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <sys/sysctl.h>

namespace darwin_utils {

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

inline std::optional<std::string> runSysctl(const std::string& key) {
  size_t len = 0;
  if (sysctlbyname(key.c_str(), nullptr, &len, nullptr, 0) != 0) {
    return std::nullopt;
  }

  std::vector<char> buffer(len + 1, 0);
  if (sysctlbyname(key.c_str(), buffer.data(), &len, nullptr, 0) != 0) {
    return std::nullopt;
  }

  std::string result(buffer.data());
  return trim(result);
}

inline std::optional<int64_t> runSysctlInt(const std::string& key) {
  int64_t value = 0;
  size_t len = sizeof(value);
  if (sysctlbyname(key.c_str(), &value, &len, nullptr, 0) != 0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<double> parseOptionalDouble(const std::string& input) {
  const std::string cleaned = trim(input);
  if (cleaned.empty() || cleaned == "N/A" || cleaned == "NA") {
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

inline std::vector<std::string> splitByChar(const std::string& line, char delimiter) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, delimiter)) {
    out.push_back(trim(item));
  }
  return out;
}

inline std::optional<double> normalizePercent(double value) {
  if (value < 0.0 || value > 100.0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<double> normalizeTemperatureC(double value) {
  if (value < 0.0 || value > 150.0) {
    return std::nullopt;
  }
  return value;
}

}  // namespace darwin_utils
