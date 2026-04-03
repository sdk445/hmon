#pragma once

#include <string>
#include <vector>

namespace hmon::plugins::systemd {

struct ServiceInfo {
    std::string name;
    std::string load_state;    /* loaded, not-found */
    std::string active_state;  /* active, inactive, failed */
    std::string sub_state;     /* running, exited, dead, failed */
    std::string description;
};

struct SystemdPluginCtx {
    std::vector<ServiceInfo> services;
};

std::vector<ServiceInfo> collectServices(SystemdPluginCtx* ctx);

}
