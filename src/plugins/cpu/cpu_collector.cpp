#include "cpu_collector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool isCpuDir(const std::string& name) {
    if (name.size() <= 3 || name.rfind("cpu", 0) != 0) return false;
    return std::all_of(name.begin() + 3, name.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool hwmonLooksCpu(const std::string& lower) {
    return lower.find("k10temp") != std::string::npos ||
           lower.find("coretemp") != std::string::npos ||
           lower.find("zenpower") != std::string::npos ||
           lower.find("cpu") != std::string::npos;
}

bool hwmonLabelLooksCpuTemp(const std::string& lower) {
    return lower.find("cpu") != std::string::npos ||
           lower.find("package") != std::string::npos ||
           lower.find("tctl") != std::string::npos ||
           lower.find("tdie") != std::string::npos ||
           lower.find("die") != std::string::npos;
}

bool hasAlpha(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::optional<long long> readLL(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    long long v;
    if (f >> v) return v;
    return std::nullopt;
}

std::optional<std::string> readFirstLine(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string line;
    if (std::getline(f, line)) return trim(line);
    return std::nullopt;
}

std::optional<double> normalizeTempC(long long raw) {
    if (raw <= 0) return std::nullopt;
    return static_cast<double>(raw) / 1000.0;
}

std::optional<double> parseDouble(const std::string& s) {
    std::string cleaned = trim(s);
    if (cleaned.empty()) return std::nullopt;
    try {
        size_t pos = 0;
        double v = std::stod(cleaned, &pos);
        if (pos != cleaned.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

}

namespace hmon::plugins::cpu {

std::string collectName() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) return "Unknown CPU";

    std::optional<std::string> model_fb;
    std::optional<std::string> proc_fb;
    std::string line;
    while (std::getline(cpuinfo, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = toLower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (value.empty()) continue;

        if (key == "model name" || key == "cpu model" || key == "hardware") {
            return value;
        }
        if (key == "model" && hasAlpha(value) && !model_fb) {
            model_fb = value;
        }
        if (key == "processor" && hasAlpha(value) && !proc_fb) {
            proc_fb = value;
        }
    }
    if (model_fb) return *model_fb;
    if (proc_fb) return *proc_fb;
    return "Unknown CPU";
}

std::optional<int> collectThreadCount() {
    const fs::path cpu_base("/sys/devices/system/cpu");
    int count = 0;

    if (fs::exists(cpu_base)) {
        for (const auto& e : fs::directory_iterator(cpu_base)) {
            if (isCpuDir(e.path().filename().string())) ++count;
        }
    }
    if (count > 0) return count;

    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) return std::nullopt;
    std::string line;
    while (std::getline(cpuinfo, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (toLower(trim(line.substr(0, colon))) == "processor") ++count;
    }
    return count > 0 ? std::optional<int>(count) : std::nullopt;
}

std::optional<int> collectCoreCount() {
    const fs::path cpu_base("/sys/devices/system/cpu");
    std::set<std::string> unique;

    if (fs::exists(cpu_base)) {
        for (const auto& e : fs::directory_iterator(cpu_base)) {
            if (!isCpuDir(e.path().filename().string())) continue;
            auto core_id = readFirstLine(e.path() / "topology" / "core_id");
            if (!core_id) continue;
            std::string pkg = readFirstLine(e.path() / "topology" / "physical_package_id").value_or("0");
            unique.insert(pkg + ":" + *core_id);
        }
    }
    if (!unique.empty()) return static_cast<int>(unique.size());

    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) return std::nullopt;

    std::set<std::string> unique2;
    std::set<std::string> phys_ids;
    std::optional<std::string> blk_phys, blk_core;
    int cores_per_socket = 0;

    auto flush = [&]() {
        if (blk_core) unique2.insert(blk_phys.value_or("0") + ":" + *blk_core);
        blk_phys.reset();
        blk_core.reset();
    };

    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (trim(line).empty()) { flush(); continue; }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (value.empty()) continue;

        if (key == "physical id") { blk_phys = value; phys_ids.insert(value); }
        else if (key == "core id") { blk_core = value; }
        else if (key == "cpu cores") {
            try { cores_per_socket = std::max(cores_per_socket, std::stoi(value)); } catch (...) {}
        }
    }
    flush();

    if (!unique2.empty()) return static_cast<int>(unique2.size());
    if (cores_per_socket > 0) return cores_per_socket * std::max(1, static_cast<int>(phys_ids.size()));
    return std::nullopt;
}

std::optional<double> collectTemperature() {
    const fs::path thermal_base("/sys/class/thermal");
    std::optional<double> preferred_max, fallback_max;

    if (fs::exists(thermal_base)) {
        for (const auto& e : fs::directory_iterator(thermal_base)) {
            std::string zone = e.path().filename().string();
            if (zone.rfind("thermal_zone", 0) != 0) continue;

            auto raw = readLL(e.path() / "temp");
            if (!raw) continue;
            auto c = normalizeTempC(*raw);
            if (!c) continue;

            std::string type = toLower(readFirstLine(e.path() / "type").value_or(""));
            bool preferred = type.find("cpu") != std::string::npos ||
                             type.find("package") != std::string::npos ||
                             type.find("x86_pkg_temp") != std::string::npos ||
                             type.find("tctl") != std::string::npos ||
                             type.find("tdie") != std::string::npos;

            if (preferred) {
                if (!preferred_max || *c > *preferred_max) preferred_max = *c;
            } else {
                if (!fallback_max || *c > *fallback_max) fallback_max = *c;
            }
        }
    }

    if (preferred_max) return preferred_max;
    if (fallback_max) return fallback_max;

    /* hwmon fallback */
    const fs::path hwmon_base("/sys/class/hwmon");
    if (!fs::exists(hwmon_base)) return std::nullopt;

    std::optional<double> hwmon_pref, hwmon_fb;
    for (const auto& hw : fs::directory_iterator(hwmon_base)) {
        std::string chip = toLower(readFirstLine(hw.path() / "name").value_or(""));
        bool cpu_chip = hwmonLooksCpu(chip);

        for (const auto& f : fs::directory_iterator(hw.path())) {
            std::string fn = f.path().filename().string();
            if (fn.rfind("temp", 0) != 0 || fn.rfind("_input") != fn.size() - 6) continue;

            auto raw = readLL(f.path());
            if (!raw) continue;
            auto c = normalizeTempC(*raw);
            if (!c) continue;

            std::string label_fn = fn;
            label_fn.replace(label_fn.size() - 6, 6, "_label");
            std::string label = toLower(readFirstLine(hw.path() / label_fn).value_or(""));
            bool cpu_label = hwmonLabelLooksCpuTemp(label);

            if (cpu_label) {
                if (!hwmon_pref || *c > *hwmon_pref) hwmon_pref = *c;
            } else if (cpu_chip) {
                if (!hwmon_fb || *c > *hwmon_fb) hwmon_fb = *c;
            }
        }
    }
    if (hwmon_pref) return hwmon_pref;
    return hwmon_fb;
}

std::optional<double> collectFrequency() {
    std::vector<double> mhz;
    const fs::path cpu_base("/sys/devices/system/cpu");

    if (fs::exists(cpu_base)) {
        for (const auto& e : fs::directory_iterator(cpu_base)) {
            if (!isCpuDir(e.path().filename().string())) continue;
            auto khz = readLL(e.path() / "cpufreq" / "scaling_cur_freq");
            if (khz && *khz > 0) mhz.push_back(static_cast<double>(*khz) / 1000.0);
        }
    }

    if (mhz.empty()) {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("cpu MHz", 0) != 0) continue;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto v = parseDouble(line.substr(colon + 1));
            if (v && *v > 0.0) mhz.push_back(*v);
        }
    }

    if (mhz.empty()) return std::nullopt;
    double sum = std::accumulate(mhz.begin(), mhz.end(), 0.0);
    return sum / static_cast<double>(mhz.size());
}

std::optional<double> collectUsagePercent(CpuPluginCtx* ctx) {
    std::ifstream stat("/proc/stat");
    if (!stat) return std::nullopt;

    std::string tag;
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    stat >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    if (tag != "cpu") return std::nullopt;

    unsigned long long idle_ticks = idle + iowait;
    unsigned long long total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;

    if (!ctx->cpu_usage_initialized || total_ticks < ctx->prev_total || idle_ticks < ctx->prev_idle) {
        ctx->prev_idle = idle_ticks;
        ctx->prev_total = total_ticks;
        ctx->cpu_usage_initialized = true;
        return std::nullopt;
    }

    unsigned long long total_delta = total_ticks - ctx->prev_total;
    unsigned long long idle_delta = idle_ticks - ctx->prev_idle;

    ctx->prev_idle = idle_ticks;
    ctx->prev_total = total_ticks;

    if (total_delta == 0) return std::nullopt;

    double usage = 100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta));
    return std::max(0.0, std::min(100.0, usage));
}

std::vector<double> collectPerCoreUsagePercent(CpuPluginCtx* ctx) {
    std::vector<double> core_usage;

    std::ifstream stat("/proc/stat");
    if (!stat) return core_usage;

    std::string line;
    std::vector<std::array<unsigned long long, 8>> cpu_values;

    while (std::getline(stat, line)) {
        if (line.size() <= 3 || line[0] != 'c' || line[1] != 'p' || line[2] != 'u' || !std::isdigit(line[3]))
            continue;

        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        std::array<unsigned long long, 8> vals{};
        bool valid = true;
        for (int i = 0; i < 8; i++) {
            if (!(iss >> vals[i])) { valid = false; break; }
        }
        if (valid) cpu_values.push_back(vals);
    }

    if (cpu_values.empty()) return core_usage;

    if (ctx->core_states.size() != cpu_values.size()) {
        ctx->core_states.resize(cpu_values.size());
        for (size_t i = 0; i < cpu_values.size(); i++) {
            ctx->core_states[i].user = cpu_values[i][0];
            ctx->core_states[i].nice = cpu_values[i][1];
            ctx->core_states[i].system = cpu_values[i][2];
            ctx->core_states[i].idle = cpu_values[i][3];
            ctx->core_states[i].iowait = cpu_values[i][4];
            ctx->core_states[i].irq = cpu_values[i][5];
            ctx->core_states[i].softirq = cpu_values[i][6];
            ctx->core_states[i].steal = cpu_values[i][7];
            ctx->core_states[i].initialized = true;
        }
        return core_usage;
    }

    core_usage.reserve(cpu_values.size());
    for (size_t i = 0; i < cpu_values.size(); i++) {
        const auto& v = cpu_values[i];
        auto& s = ctx->core_states[i];

        unsigned long long total_delta =
            (v[0] - s.user) + (v[1] - s.nice) + (v[2] - s.system) +
            (v[3] - s.idle) + (v[4] - s.iowait) + (v[5] - s.irq) +
            (v[6] - s.softirq) + (v[7] - s.steal);
        unsigned long long idle_total = (v[3] - s.idle) + (v[4] - s.iowait);

        s.user = v[0]; s.nice = v[1]; s.system = v[2]; s.idle = v[3];
        s.iowait = v[4]; s.irq = v[5]; s.softirq = v[6]; s.steal = v[7];

        if (total_delta == 0) {
            core_usage.push_back(0.0);
        } else {
            double usage = 100.0 * (1.0 - static_cast<double>(idle_total) / static_cast<double>(total_delta));
            core_usage.push_back(std::max(0.0, std::min(100.0, usage)));
        }
    }

    return core_usage;
}

} /* namespace hmon::plugins::cpu */
