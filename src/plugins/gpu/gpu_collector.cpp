#include "gpu_collector.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

std::vector<std::string> splitByChar(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, delim)) {
        tokens.push_back(trim(token));
    }
    return tokens;
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

std::optional<double> normalizePercent(long long raw) {
    if (raw < 0) return std::nullopt;
    return std::min(100.0, static_cast<double>(raw));
}

std::string runCommand(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    pclose(pipe);
    return result;
}

std::string vendorName(const std::optional<std::string>& vid) {
    if (!vid) return "Unknown";
    std::string lower = toLower(*vid);
    if (lower == "0x10de") return "NVIDIA";
    if (lower == "0x1002") return "AMD";
    if (lower == "0x8086") return "Intel";
    return "Vendor " + *vid;
}

bool isDisplayClass(const fs::path& dev) {
    auto cls = readFirstLine(dev / "class");
    if (!cls) return true;
    return toLower(*cls).rfind("0x03", 0) == 0;
}

std::optional<std::string> readDriver(const fs::path& dev) {
    std::error_code ec;
    fs::path link = fs::read_symlink(dev / "driver", ec);
    if (!ec && !link.filename().string().empty()) {
        return link.filename().string();
    }
    std::ifstream uevent(dev / "uevent");
    if (!uevent) return std::nullopt;
    std::string line;
    while (std::getline(uevent, line)) {
        if (line.rfind("DRIVER=", 0) == 0) {
            std::string d = trim(line.substr(7));
            if (!d.empty()) return d;
            break;
        }
    }
    return std::nullopt;
}

std::optional<bool> detectInUse(const fs::path& drm, const fs::path& card, const fs::path& dev) {
    std::string prefix = card.filename().string() + "-";
    bool saw_status = false;
    for (const auto& e : fs::directory_iterator(drm)) {
        std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (name.find("render") != std::string::npos) continue;
        auto status = readFirstLine(e.path() / "status");
        if (!status) continue;
        saw_status = true;
        if (toLower(*status) == "connected") return true;
    }
    if (saw_status) return false;
    auto boot = readLL(dev / "boot_vga");
    if (boot) return *boot == 1;
    return std::nullopt;
}

int score(const hmon::plugins::gpu::GpuInfo& g) {
    int s = 0;
    if (g.temperature_c) s += 2;
    if (g.core_clock_mhz) s += 2;
    if (g.utilization_percent) s += 3;
    if (g.power_w) s += 2;
    if (g.memory_used_mib || g.memory_total_mib) s += 2;
    if (g.memory_utilization_percent) ++s;
    return s;
}

bool looksNvidia(const hmon::plugins::gpu::GpuInfo& g) {
    std::string n = toLower(g.name);
    std::string src = toLower(g.source);
    return n.find("nvidia") != std::string::npos || src.find("nvidia") != std::string::npos;
}

std::optional<double> readAmdClock(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::regex pattern(R"(:\s*([0-9]+(?:\.[0-9]+)?)\s*[Mm][Hh][Zz].*\*)");
    std::string line;
    while (std::getline(f, line)) {
        std::smatch match;
        if (std::regex_search(line, match, pattern)) {
            try { return std::stod(match[1].str()); } catch (...) { return std::nullopt; }
        }
    }
    return std::nullopt;
}

std::vector<double> amdCoreUsage(const fs::path& dev) {
    std::vector<double> result;
    auto gpu_busy = readLL(dev / "gpu_busy_percent");
    if (gpu_busy) {
        auto n = normalizePercent(*gpu_busy);
        if (n) result.push_back(*n);
    }
    auto mem_busy = readLL(dev / "mem_busy_percent");
    if (mem_busy) {
        auto n = normalizePercent(*mem_busy);
        if (n) result.push_back(*n);
    }
    return result;
}

std::optional<double> readHwmonPower(const fs::path& sensor) {
    auto uw = readLL(sensor / "power1_average");
    if (!uw) uw = readLL(sensor / "power1_input");
    if (!uw) uw = readLL(sensor / "power2_average");
    if (!uw) uw = readLL(sensor / "power2_input");
    if (uw && *uw > 0) return static_cast<double>(*uw) / 1000000.0;
    return std::nullopt;
}

std::optional<double> readSensorsPower() {
    std::string output = runCommand("sensors -j 2>/dev/null");
    if (output.empty()) return std::nullopt;
    /* Simple grep for power lines in sensors output */
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("power") != std::string::npos && line.find("W") != std::string::npos) {
            /* Try to extract a numeric value */
            for (size_t i = 0; i < line.size(); i++) {
                if (std::isdigit(line[i]) || line[i] == '.') {
                    try {
                        size_t pos = 0;
                        double v = std::stod(line.substr(i), &pos);
                        if (v > 0 && v < 10000) return v;
                    } catch (...) {}
                    break;
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<hmon::plugins::gpu::GpuInfo> fromNvidiaSmi() {
    std::vector<hmon::plugins::gpu::GpuInfo> result;
    std::string cmd =
        "nvidia-smi --query-gpu=name,temperature.gpu,clocks.sm,utilization.gpu,power.draw,"
        "memory.used,memory.total,memory.free,utilization.memory "
        "--format=csv,noheader,nounits 2>/dev/null";
    std::string output = runCommand(cmd);
    if (trim(output).empty()) return result;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (trim(line).empty()) continue;
        auto fields = splitByChar(line, ',');
        if (fields.size() < 9) continue;

        hmon::plugins::gpu::GpuInfo g;
        g.name = fields[0];
        g.source = "nvidia-smi";
        g.temperature_c = parseDouble(fields[1]);
        g.core_clock_mhz = parseDouble(fields[2]);
        g.utilization_percent = parseDouble(fields[3]);
        g.power_w = parseDouble(fields[4]);
        g.memory_used_mib = parseDouble(fields[5]);
        g.memory_total_mib = parseDouble(fields[6]);

        if (g.memory_used_mib && g.memory_total_mib && *g.memory_total_mib > 0.0) {
            g.memory_utilization_percent = 100.0 * (*g.memory_used_mib) / (*g.memory_total_mib);
        }
        auto mem_util = parseDouble(fields[8]);
        if (mem_util) g.memory_utilization_percent = mem_util;

        g.gpu_core_usage_percent = {g.utilization_percent.value_or(0.0)};
        if (g.memory_utilization_percent) {
            g.gpu_core_usage_percent.push_back(*g.memory_utilization_percent);
        }

        result.push_back(std::move(g));
    }
    return result;
}

std::vector<hmon::plugins::gpu::GpuInfo> fromSysfs(std::optional<double> sensors_power) {
    std::vector<hmon::plugins::gpu::GpuInfo> result;
    const fs::path drm("/sys/class/drm");
    if (!fs::exists(drm)) return result;

    for (const auto& card : fs::directory_iterator(drm)) {
        std::string name = card.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;

        fs::path dev = card.path() / "device";
        if (!fs::exists(dev) || !isDisplayClass(dev)) continue;

        auto vendor = readFirstLine(dev / "vendor");
        auto driver = readDriver(dev);

        hmon::plugins::gpu::GpuInfo g;
        g.source = driver ? ("sysfs/" + *driver) : "sysfs";
        g.name = name + " (" + vendorName(vendor) + ")";
        g.in_use = detectInUse(drm, card.path(), dev);

        if (fs::exists(dev / "hwmon")) {
            for (const auto& hw : fs::directory_iterator(dev / "hwmon")) {
                if (!fs::is_directory(hw.path())) continue;
                if (!g.power_w) g.power_w = readHwmonPower(hw.path());
                for (const auto& f : fs::directory_iterator(hw.path())) {
                    std::string fn = f.path().filename().string();
                    if (!g.temperature_c && fn.rfind("temp", 0) == 0 && fn.rfind("_input") == fn.size() - 6) {
                        auto raw = readLL(f.path());
                        if (raw) g.temperature_c = static_cast<double>(*raw) / 1000.0;
                    }
                }
            }
        }

        auto clk = readLL(dev / "gt_cur_freq_mhz");
        if (clk && *clk > 0) {
            g.core_clock_mhz = static_cast<double>(*clk);
        } else {
            g.core_clock_mhz = readAmdClock(dev / "pp_dpm_sclk");
        }

        auto util = readLL(dev / "gpu_busy_percent");
        if (util) g.utilization_percent = normalizePercent(*util);

        g.gpu_core_usage_percent = amdCoreUsage(dev);

        auto vram_used = readLL(dev / "mem_info_vram_used");
        if (!vram_used) vram_used = readLL(dev / "mem_info_vis_vram_used");
        auto vram_total = readLL(dev / "mem_info_vram_total");
        if (!vram_total) vram_total = readLL(dev / "mem_info_vis_vram_total");

        if (vram_used && vram_total && *vram_total > 0) {
            g.memory_used_mib = static_cast<double>(*vram_used) / (1024.0 * 1024.0);
            g.memory_total_mib = static_cast<double>(*vram_total) / (1024.0 * 1024.0);
            if (*g.memory_total_mib > 0.0) {
                g.memory_utilization_percent = 100.0 * (*g.memory_used_mib) / (*g.memory_total_mib);
            }
        }

        if (!g.power_w && sensors_power) g.power_w = sensors_power;

        result.push_back(std::move(g));
    }

    std::stable_sort(result.begin(), result.end(),
                     [](const auto& a, const auto& b) {
                         int sa = score(a), sb = score(b);
                         if (sa != sb) return sa > sb;
                         return a.name < b.name;
                     });
    return result;
}

}

namespace hmon::plugins::gpu {

std::vector<GpuInfo> collectGpus() {
    auto nvidia = fromNvidiaSmi();
    auto sensors_power = readSensorsPower();
    auto sysfs = fromSysfs(sensors_power);

    if (!nvidia.empty()) {
        std::vector<bool> sysfs_used(sysfs.size(), false);
        std::vector<size_t> nvidia_sysfs;
        for (size_t i = 0; i < sysfs.size(); ++i) {
            if (looksNvidia(sysfs[i])) nvidia_sysfs.push_back(i);
        }

        size_t ni = 0, ai = 0;
        for (auto& base : nvidia) {
            const GpuInfo* extra = nullptr;
            while (ni < nvidia_sysfs.size()) {
                size_t idx = nvidia_sysfs[ni++];
                if (!sysfs_used[idx]) { sysfs_used[idx] = true; extra = &sysfs[idx]; break; }
            }
            if (!extra) {
                while (ai < sysfs.size()) {
                    size_t idx = ai++;
                    if (!sysfs_used[idx]) { sysfs_used[idx] = true; extra = &sysfs[idx]; break; }
                }
            }
            if (!extra) continue;

            if (!base.temperature_c) base.temperature_c = extra->temperature_c;
            if (!base.core_clock_mhz) base.core_clock_mhz = extra->core_clock_mhz;
            if (!base.utilization_percent) base.utilization_percent = extra->utilization_percent;
            if (!base.power_w) base.power_w = extra->power_w;
            if (!base.memory_used_mib) base.memory_used_mib = extra->memory_used_mib;
            if (!base.memory_total_mib) base.memory_total_mib = extra->memory_total_mib;
            if (!base.memory_utilization_percent) base.memory_utilization_percent = extra->memory_utilization_percent;
            if (!base.in_use.has_value() && extra->in_use.has_value()) base.in_use = extra->in_use;
        }

        for (auto& g : nvidia) {
            if (!g.power_w && sensors_power) g.power_w = sensors_power;
            if (!g.memory_utilization_percent && g.memory_used_mib && g.memory_total_mib && *g.memory_total_mib > 0.0) {
                g.memory_utilization_percent = 100.0 * (*g.memory_used_mib) / (*g.memory_total_mib);
            }
        }

        for (size_t i = 0; i < sysfs.size(); ++i) {
            if (!sysfs_used[i]) nvidia.push_back(sysfs[i]);
        }

        std::stable_sort(nvidia.begin(), nvidia.end(),
                         [](const auto& a, const auto& b) {
                             int sa = score(a), sb = score(b);
                             if (sa != sb) return sa > sb;
                             return a.name < b.name;
                         });
        return nvidia;
    }

    return sysfs;
}

} /* namespace hmon::plugins::gpu */
