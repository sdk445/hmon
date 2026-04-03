#include "cron_collector.hpp"

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

static void parseCrontab(const std::string& path, const std::string& source,
                          const std::string& default_user,
                          std::vector<hmon::plugins::cron::CronJob>& jobs) {
    std::ifstream f(path);
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("SHELL=", 0) == 0 || line.rfind("PATH=", 0) == 0 ||
            line.rfind("MAILTO=", 0) == 0 || line.rfind("HOME=", 0) == 0) continue;

        /* System crontab format: min hour dom month dow user command */
        /* User crontab format: min hour dom month dow command */
        std::istringstream iss(line);
        std::string min, hour, dom, month, dow, user_or_cmd;
        iss >> min >> hour >> dom >> month >> dow >> user_or_cmd;
        if (min.empty() || hour.empty()) continue;

        hmon::plugins::cron::CronJob job;
        job.schedule = min + " " + hour + " " + dom + " " + month + " " + dow;
        job.source = source;

        /* Check if next field looks like a username (system crontab) */
        std::string rest;
        std::getline(iss, rest);
        rest = trim(rest);

        /* Heuristic: if there's a space after user_or_cmd, it's a system crontab */
        if (!rest.empty() && user_or_cmd.find('/') == std::string::npos &&
            user_or_cmd.find('.') == std::string::npos && user_or_cmd.find('=') == std::string::npos) {
            job.user = user_or_cmd;
            job.command = rest;
        } else {
            job.user = default_user;
            job.command = user_or_cmd + " " + rest;
        }
        job.command = trim(job.command);

        if (!job.command.empty()) {
            jobs.push_back(std::move(job));
        }
    }
}

}

namespace hmon::plugins::cron {

std::vector<CronJob> collectCronJobs(CronPluginCtx* /*ctx*/) {
    std::vector<CronJob> jobs;

    /* /etc/crontab */
    parseCrontab("/etc/crontab", "/etc/crontab", "root", jobs);

    /* /etc/cron.d/* */
    std::error_code ec;
    if (fs::exists("/etc/cron.d", ec)) {
        for (const auto& entry : fs::directory_iterator("/etc/cron.d", ec)) {
            if (entry.is_regular_file()) {
                parseCrontab(entry.path().string(), entry.path().filename().string(), "root", jobs);
            }
        }
    }

    /* User crontabs in /var/spool/cron/ or /var/spool/cron/crontabs/ */
    std::string crontab_dir = "/var/spool/cron";
    if (!fs::exists(crontab_dir, ec)) {
        crontab_dir = "/var/spool/cron/crontabs";
    }
    if (fs::exists(crontab_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(crontab_dir, ec)) {
            if (entry.is_regular_file()) {
                std::string user = entry.path().filename().string();
                parseCrontab(entry.path().string(), "user:" + user, user, jobs);
            }
        }
    }

    return jobs;
}

}
