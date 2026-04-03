#include "systemd_collector.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string parseDescriptionFromFile(const fs::path& unit_file) {
    std::ifstream f(unit_file);
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Description=", 0) == 0) {
            return trim(line.substr(12));
        }
    }
    return "";
}

static std::string resolveServiceDescription(const std::string& unit_name) {
    static std::vector<fs::path> search_dirs = {
        "/run/systemd/system",
        "/etc/systemd/system",
        "/usr/lib/systemd/system",
        "/lib/systemd/system"
    };
    for (const auto& dir : search_dirs) {
        fs::path p = dir / (unit_name + ".service");
        if (fs::exists(p)) {
            std::string desc = parseDescriptionFromFile(p);
            if (!desc.empty()) return desc;
        }
    }
    return unit_name;
}

static void scanUnitFiles(std::vector<std::string>& out_unit_names) {
    static std::vector<fs::path> search_dirs = {
        "/run/systemd/system",
        "/etc/systemd/system",
        "/usr/lib/systemd/system",
        "/lib/systemd/system"
    };
    for (const auto& dir : search_dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        fs::directory_iterator it(dir, ec);
        if (ec) continue;
        for (const auto& entry : it) {
            std::string fname = entry.path().filename().string();
            if (fname.size() < 9) continue;
            if (fname.rfind(".service") != fname.size() - 8) continue;
            std::string unit = fname.substr(0, fname.size() - 8);
            if (std::find(out_unit_names.begin(), out_unit_names.end(), unit) == out_unit_names.end()) {
                out_unit_names.push_back(unit);
            }
        }
    }
}

static bool serviceHasProcs(const std::string& unit_name) {
    std::vector<fs::path> cgroup_paths = {
        fs::path("/sys/fs/cgroup/system.slice") / (unit_name + ".service") / "cgroup.procs",
        fs::path("/sys/fs/cgroup/systemd/system.slice") / (unit_name + ".service") / "cgroup.procs",
        fs::path("/sys/fs/cgroup") / (unit_name + ".service") / "cgroup.procs",
    };
    for (const auto& p : cgroup_paths) {
        std::ifstream f(p);
        if (!f) continue;
        std::string line;
        if (std::getline(f, line) && !trim(line).empty()) return true;
    }
    return false;
}

static bool serviceIsFailed(const std::string& unit_name) {
    std::string path = "/run/systemd/units/invocation:" + unit_name + ".service";
    if (!fs::exists(path)) return false;
    std::ifstream f(path);
    if (!f) return false;
    std::string content;
    std::getline(f, content);
    content = trim(content);
    return content == "failed" || content.find("failed") != std::string::npos;
}

static bool isSystemdRunning() {
    return fs::exists("/run/systemd/system");
}

}

namespace hmon::plugins::systemd {

std::vector<ServiceInfo> collectServices(SystemdPluginCtx* ctx) {
    if (ctx) {
        auto elapsed = std::chrono::steady_clock::now() - ctx->last_cache_time;
        if (!ctx->cached_result.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < SystemdPluginCtx::TTL_SECONDS) {
            return ctx->cached_result;
        }
    }

    std::vector<ServiceInfo> result;

    if (!isSystemdRunning()) {
        if (ctx) {
            ctx->cached_result = result;
            ctx->last_cache_time = std::chrono::steady_clock::now();
        }
        return result;
    }

    std::vector<std::string> unit_names;
    scanUnitFiles(unit_names);

    for (const auto& unit : unit_names) {
        ServiceInfo si;
        si.name = unit;
        si.description = resolveServiceDescription(unit);
        si.load_state = "loaded";

        if (serviceIsFailed(unit)) {
            si.active_state = "failed";
            si.sub_state = "failed";
        } else if (serviceHasProcs(unit)) {
            si.active_state = "active";
            si.sub_state = "running";
        }

        if (si.active_state == "active" || si.active_state == "failed") {
            result.push_back(std::move(si));
        }
    }

    std::sort(result.begin(), result.end(),
              [](const ServiceInfo& a, const ServiceInfo& b) {
                  bool a_failed = (a.active_state == "failed");
                  bool b_failed = (b.active_state == "failed");
                  if (a_failed != b_failed) return a_failed > b_failed;
                  return a.name < b.name;
              });

    if (ctx) {
        ctx->cached_result = result;
        ctx->last_cache_time = std::chrono::steady_clock::now();
    }
    return result;
}

}
