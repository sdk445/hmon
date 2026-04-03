#include "systemd_collector.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}

namespace hmon::plugins::systemd {

std::vector<ServiceInfo> collectServices(SystemdPluginCtx* /*ctx*/) {
    std::vector<ServiceInfo> result;

    /* Use systemctl with machine-readable output */
    FILE* pipe = popen(
        "systemctl list-units --type=service --state=active,failed --no-pager --no-legend --plain 2>/dev/null",
        "r");
    if (!pipe) return result;

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line = buf;
        /* Format: UNIT LOAD ACTIVE SUB DESCRIPTION */
        /* UNIT is like nginx.service, LOAD=loaded, ACTIVE=active/failed, SUB=running/exited/failed */
        std::istringstream iss(line);
        std::string unit, load, active, sub;
        iss >> unit >> load >> active >> sub;
        if (unit.empty() || unit == "0") continue;

        /* Get description from rest of line */
        std::string desc;
        std::getline(iss, desc);
        desc = trim(desc);

        ServiceInfo si;
        si.name = unit;
        /* Remove .service suffix for display */
        if (si.name.size() > 8 && si.name.rfind(".service") == si.name.size() - 8) {
            si.name = si.name.substr(0, si.name.size() - 8);
        }
        si.load_state = load;
        si.active_state = active;
        si.sub_state = sub;
        si.description = desc.empty() ? si.name : desc;
        result.push_back(std::move(si));
    }
    pclose(pipe);

    /* Sort: failed first, then alphabetically */
    std::sort(result.begin(), result.end(),
              [](const ServiceInfo& a, const ServiceInfo& b) {
                  bool a_failed = (a.active_state == "failed");
                  bool b_failed = (b.active_state == "failed");
                  if (a_failed != b_failed) return a_failed > b_failed;
                  return a.name < b.name;
              });

    return result;
}

}
