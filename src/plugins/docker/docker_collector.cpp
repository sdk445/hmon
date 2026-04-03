#include "docker_collector.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

static size_t skipJsonValue(const std::string& data, size_t pos) {
    if (pos >= data.size()) return pos;
    char c = data[pos];
    if (c == '"') {
        ++pos;
        while (pos < data.size()) {
            if (data[pos] == '\\') { ++pos; if (pos < data.size()) ++pos; continue; }
            if (data[pos] == '"') { ++pos; return pos; }
            ++pos;
        }
        return pos;
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        while (pos < data.size()) {
            char ch = data[pos];
            if (ch == '"' && (pos == 0 || data[pos - 1] != '\\')) in_str = !in_str;
            if (!in_str) {
                if (ch == open) ++depth;
                else if (ch == close) { --depth; if (depth == 0) return pos + 1; }
            }
            ++pos;
        }
        return pos;
    }
    while (pos < data.size() && data[pos] != ',' && data[pos] != '}' && data[pos] != ']' &&
           data[pos] != ' ' && data[pos] != '\n' && data[pos] != '\r' && data[pos] != '\t')
        ++pos;
    return pos;
}

static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = 0;
    while (pos < json.size()) {
        size_t k = json.find(search, pos);
        if (k == std::string::npos) return "";
        size_t before = k;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t' ||
               json[before - 1] == '\n' || json[before - 1] == '\r')) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',' && json[before - 1] != ':')) {
            pos = k + 1;
            continue;
        }
        size_t val_start = k + search.size();
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != ':') { pos = k + 1; continue; }
        ++val_start;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != '"') return "";
        ++val_start;
        std::string result;
        while (val_start < json.size() && json[val_start] != '"') {
            if (json[val_start] == '\\' && val_start + 1 < json.size()) {
                ++val_start;
                switch (json[val_start]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    default: result += json[val_start]; break;
                }
            } else {
                result += json[val_start];
            }
            ++val_start;
        }
        return result;
    }
    return "";
}

static std::string extractArrayString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = 0;
    while (pos < json.size()) {
        size_t k = json.find(search, pos);
        if (k == std::string::npos) return "";
        size_t before = k;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t' ||
               json[before - 1] == '\n' || json[before - 1] == '\r')) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',' && json[before - 1] != ':')) {
            pos = k + 1;
            continue;
        }
        size_t val_start = k + search.size();
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != ':') { pos = k + 1; continue; }
        ++val_start;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != '[') return "";
        ++val_start;
        while (val_start < json.size() && json[val_start] != ']') {
            if (json[val_start] == '"') {
                ++val_start;
                std::string result;
                while (val_start < json.size() && json[val_start] != '"') {
                    if (json[val_start] == '\\' && val_start + 1 < json.size()) ++val_start;
                    result += json[val_start];
                    ++val_start;
                }
                return result;
            }
            ++val_start;
        }
        return "";
    }
    return "";
}

static uint64_t extractUint64(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = 0;
    while (pos < json.size()) {
        size_t k = json.find(search, pos);
        if (k == std::string::npos) return 0;
        size_t before = k;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t' ||
               json[before - 1] == '\n' || json[before - 1] == '\r')) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',' && json[before - 1] != ':')) {
            pos = k + 1;
            continue;
        }
        size_t val_start = k + search.size();
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != ':') { pos = k + 1; continue; }
        ++val_start;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size()) return 0;
        char c = json[val_start];
        if (c != '-' && !(c >= '0' && c <= '9')) { pos = k + 1; continue; }
        size_t num_start = val_start;
        if (c == '-') ++val_start;
        while (val_start < json.size() && json[val_start] >= '0' && json[val_start] <= '9') ++val_start;
        try { return std::stoull(json.substr(num_start, val_start - num_start)); } catch (...) { return 0; }
    }
    return 0;
}

static int extractInt(const std::string& json, const std::string& key) {
    try { return static_cast<int>(extractUint64(json, key)); } catch (...) { return 0; }
}

static std::string extractObject(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = 0;
    while (pos < json.size()) {
        size_t k = json.find(search, pos);
        if (k == std::string::npos) return "";
        size_t before = k;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t' ||
               json[before - 1] == '\n' || json[before - 1] == '\r')) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',' && json[before - 1] != ':')) {
            pos = k + 1;
            continue;
        }
        size_t val_start = k + search.size();
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != ':') { pos = k + 1; continue; }
        ++val_start;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != '{') return "";
        int depth = 0;
        bool in_str = false;
        size_t obj_start = val_start;
        for (size_t i = val_start; i < json.size(); ++i) {
            char ch = json[i];
            if (ch == '"' && (i == 0 || json[i - 1] != '\\')) in_str = !in_str;
            if (!in_str) {
                if (ch == '{') ++depth;
                else if (ch == '}') { --depth; if (depth == 0) return json.substr(obj_start, i - obj_start + 1); }
            }
        }
        return "";
    }
    return "";
}

static void forEachTopLevelArray(const std::string& json, const std::function<void(const std::string&)>& cb) {
    size_t pos = 0;
    while (pos < json.size() && json[pos] != '[') ++pos;
    if (pos >= json.size()) return;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ',')) ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        size_t start = pos;
        pos = skipJsonValue(json, pos);
        cb(json.substr(start, pos - start));
    }
}

static void forEachInArray(const std::string& json, const std::string& key, const std::function<void(const std::string&)>& cb) {
    std::string search = "\"" + key + "\"";
    size_t pos = 0;
    while (pos < json.size()) {
        size_t k = json.find(search, pos);
        if (k == std::string::npos) return;
        size_t before = k;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t' ||
               json[before - 1] == '\n' || json[before - 1] == '\r')) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',' && json[before - 1] != ':')) {
            pos = k + 1;
            continue;
        }
        size_t val_start = k + search.size();
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != ':') { pos = k + 1; continue; }
        ++val_start;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
               json[val_start] == '\n' || json[val_start] == '\r')) ++val_start;
        if (val_start >= json.size() || json[val_start] != '[') return;
        ++val_start;
        while (val_start < json.size()) {
            while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' || json[val_start] == '\n' || json[val_start] == '\r' || json[val_start] == ',')) ++val_start;
            if (val_start >= json.size() || json[val_start] == ']') return;
            size_t start = val_start;
            val_start = skipJsonValue(json, val_start);
            cb(json.substr(start, val_start - start));
        }
        return;
    }
}

static std::string httpGetUnixSocket(const std::string& socket_path, const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (send(fd, request.c_str(), request.size(), MSG_NOSIGNAL) < 0) {
        close(fd);
        return "";
    }

    std::string response;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return response;
    std::string body = response.substr(body_start + 4);

    size_t first_cr = body.find('\r');
    if (first_cr != std::string::npos && first_cr > 0 && first_cr < 8) {
        bool looks_chunked = true;
        for (size_t i = 0; i < first_cr; ++i) {
            char c = body[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                looks_chunked = false;
                break;
            }
        }
        if (looks_chunked) {
            std::string result;
            size_t p = 0;
            while (p < body.size()) {
                size_t cr = body.find('\r', p);
                if (cr == std::string::npos) break;
                std::string hex_size = body.substr(p, cr - p);
                size_t chunk_size = 0;
                try { chunk_size = std::stoul(hex_size, nullptr, 16); } catch (...) { break; }
                if (chunk_size == 0) break;
                size_t data_start = cr + 2;
                if (data_start + chunk_size > body.size()) break;
                result.append(body, data_start, chunk_size);
                p = data_start + chunk_size + 2;
            }
            return result;
        }
    }

    return body;
}

static std::vector<hmon::plugins::docker::ContainerStats> doCollect(const std::string& sock,
                                              std::vector<hmon::plugins::docker::ContainerStats>& prev) {
    std::vector<hmon::plugins::docker::ContainerStats> result;

    std::string containers_json = httpGetUnixSocket(sock, "/containers/json");
    if (containers_json.empty()) return prev;

    forEachTopLevelArray(containers_json, [&](const std::string& elem) {
        hmon::plugins::docker::ContainerStats cs;
        cs.id = extractString(elem, "Id");
        cs.name = extractArrayString(elem, "Names");
        if (!cs.name.empty() && cs.name[0] == '/') cs.name = cs.name.substr(1);
        cs.image = extractString(elem, "Image");
        cs.state = extractString(elem, "State");
        if (!cs.id.empty()) result.push_back(std::move(cs));
    });

    if (result.empty()) return prev;

    for (auto& cs : result) {
        std::string stats_json = httpGetUnixSocket(sock, "/containers/" + cs.id + "/stats?stream=false");
        if (stats_json.empty()) continue;

        std::string cpu_obj = extractObject(stats_json, "cpu_stats");
        if (!cpu_obj.empty()) {
            cs.system_cpu_usage = extractUint64(cpu_obj, "system_cpu_usage");
            cs.online_cpus = extractInt(cpu_obj, "online_cpus");
            std::string usage_obj = extractObject(cpu_obj, "cpu_usage");
            if (!usage_obj.empty()) {
                cs.cpu_usage_total = extractUint64(usage_obj, "total_usage");
            }
        }

        std::string precpu_obj = extractObject(stats_json, "precpu_stats");
        if (!precpu_obj.empty()) {
            cs.prev_system_cpu_usage = extractUint64(precpu_obj, "system_cpu_usage");
            std::string pusage_obj = extractObject(precpu_obj, "cpu_usage");
            if (!pusage_obj.empty()) {
                cs.prev_cpu_usage_total = extractUint64(pusage_obj, "total_usage");
            }
        }

        if (cs.cpu_usage_total > 0 && cs.system_cpu_usage > 0 &&
            cs.prev_cpu_usage_total > 0 && cs.prev_system_cpu_usage > 0) {
            uint64_t cpu_delta = cs.cpu_usage_total - cs.prev_cpu_usage_total;
            uint64_t sys_delta = cs.system_cpu_usage - cs.prev_system_cpu_usage;
            if (sys_delta > 0) {
                cs.cpu_percent = 100.0 * static_cast<double>(cpu_delta) / static_cast<double>(sys_delta);
            }
        }
        cs.cpu_initialized = true;

        std::string mem_obj = extractObject(stats_json, "memory_stats");
        if (!mem_obj.empty()) {
            cs.mem_usage = extractUint64(mem_obj, "usage");
            cs.mem_limit = extractUint64(mem_obj, "limit");
            std::string stats_str = extractObject(mem_obj, "stats");
            if (!stats_str.empty()) {
                cs.mem_cache = extractUint64(stats_str, "total_inactive_file");
            }
            if (cs.mem_limit > 0) {
                cs.mem_percent = 100.0 * static_cast<double>(cs.mem_usage) / static_cast<double>(cs.mem_limit);
            }
        }

        uint64_t total_rx = 0, total_tx = 0;
        std::string net_obj = extractObject(stats_json, "networks");
        if (!net_obj.empty()) {
            /* networks is an object {"eth0": {...}, ...}, iterate each value */
            size_t pos = 1; /* skip opening { */
            while (pos < net_obj.size()) {
                while (pos < net_obj.size() && net_obj[pos] != '"') ++pos;
                if (pos >= net_obj.size()) break;
                /* skip key string */
                ++pos;
                while (pos < net_obj.size() && net_obj[pos] != '"') {
                    if (net_obj[pos] == '\\' && pos + 1 < net_obj.size()) ++pos;
                    ++pos;
                }
                if (pos < net_obj.size()) ++pos; /* closing quote */
                /* skip colon and whitespace */
                while (pos < net_obj.size() && (net_obj[pos] == ' ' || net_obj[pos] == ':' || net_obj[pos] == '\t' || net_obj[pos] == '\n' || net_obj[pos] == '\r')) ++pos;
                if (pos >= net_obj.size()) break;
                /* extract the value object */
                size_t val_start = pos;
                pos = skipJsonValue(net_obj, pos);
                std::string iface = net_obj.substr(val_start, pos - val_start);
                total_rx += extractUint64(iface, "rx_bytes");
                total_tx += extractUint64(iface, "tx_bytes");
                /* skip comma */
                while (pos < net_obj.size() && (net_obj[pos] == ',' || net_obj[pos] == ' ' || net_obj[pos] == '\t' || net_obj[pos] == '\n' || net_obj[pos] == '\r')) ++pos;
            }
        }

        /* Docker reports cumulative bytes since container start — use directly as totals. */
        cs.net_rx_total = total_rx;
        cs.net_tx_total = total_tx;
        cs.net_rx_bytes = total_rx;
        cs.net_tx_bytes = total_tx;

        if (cs.net_initialized) {
            cs.net_rx_bps = static_cast<double>(total_rx - cs.prev_net_rx);
            cs.net_tx_bps = static_cast<double>(total_tx - cs.prev_net_tx);
        }
        cs.prev_net_rx = total_rx;
        cs.prev_net_tx = total_tx;
        cs.net_initialized = true;

        uint64_t blk_read = 0, blk_write = 0;
        std::string blkio = extractObject(stats_json, "blkio_stats");
        if (!blkio.empty()) {
            forEachInArray(blkio, "io_service_bytes_recursive", [&](const std::string& entry) {
                std::string op = extractString(entry, "Op");
                uint64_t val = extractUint64(entry, "Value");
                if (op == "Read") blk_read += val;
                else if (op == "Write") blk_write += val;
            });
        }
        cs.blk_read_bytes = blk_read;
        cs.blk_write_bytes = blk_write;

        if (cs.blk_initialized) {
            cs.blk_read_bps = static_cast<double>(blk_read - cs.prev_blk_read);
            cs.blk_write_bps = static_cast<double>(blk_write - cs.prev_blk_write);
        }
        cs.prev_blk_read = blk_read;
        cs.prev_blk_write = blk_write;
        cs.blk_initialized = true;

        std::string pids_obj = extractObject(stats_json, "pids_stats");
        if (!pids_obj.empty()) {
            cs.pids_current = extractInt(pids_obj, "current");
        }
    }

    return result;
}

}

namespace hmon::plugins::docker {

void startBackgroundCollector(DockerPluginCtx* ctx) {
    if (ctx->running) return;
    ctx->running = true;
    ctx->worker = std::thread([ctx]() {
        while (ctx->running) {
            auto fresh = doCollect(ctx->socket_path, ctx->containers);
            {
                std::lock_guard<std::mutex> lock(ctx->data_mutex);
                ctx->containers = std::move(fresh);
                if (!ctx->containers.empty()) ctx->has_data = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(DockerPluginCtx::CACHE_INTERVAL_MS));
        }
    });
    ctx->worker.detach();
}

void stopBackgroundCollector(DockerPluginCtx* ctx) {
    ctx->running = false;
}

}
