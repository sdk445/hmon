#include "hmon/plugin_manager.hpp"

#include <algorithm>
#include <dlfcn.h>
#include <dirent.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace hmon::core {

PluginManager::PluginManager() = default;
PluginManager::~PluginManager() { destroy_all(); }

int PluginManager::load_static() {
    for (const auto& sp : staticPlugins()) {
        Plugin p;
        p.name = sp.name;
        p.path = "<static>";
        p.dl_handle = nullptr;
        p.ctx = nullptr;
        p.init = sp.init;
        p.collect = sp.collect;
        p.destroy = sp.destroy;
        p.free_list = sp.free_list;
        p.control_fn = sp.control;
        plugins_.push_back(std::move(p));
    }
    return static_cast<int>(staticPlugins().size());
}

int PluginManager::load(const std::string& so_path) {
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) { std::cerr << "[hmon] dlopen(" << so_path << "): " << dlerror() << "\n"; return -1; }
    auto abi_fn = reinterpret_cast<hmon_plugin_abi_version_fn>(dlsym(handle, "hmon_plugin_abi_version"));
    if (!abi_fn) { std::cerr << "[hmon] " << so_path << ": missing hmon_plugin_abi_version\n"; dlclose(handle); return -1; }
    if (abi_fn() != HMON_PLUGIN_ABI_VERSION) { std::cerr << "[hmon] " << so_path << ": ABI mismatch\n"; dlclose(handle); return -1; }
    auto name_fn = reinterpret_cast<hmon_plugin_name_fn>(dlsym(handle, "hmon_plugin_name"));
    auto init_fn = reinterpret_cast<hmon_plugin_init_fn>(dlsym(handle, "hmon_plugin_init"));
    auto collect_fn = reinterpret_cast<hmon_plugin_collect_fn>(dlsym(handle, "hmon_plugin_collect"));
    auto destroy_fn = reinterpret_cast<hmon_plugin_destroy_fn>(dlsym(handle, "hmon_plugin_destroy"));
    auto free_list_fn = reinterpret_cast<hmon_plugin_free_list_fn>(dlsym(handle, "hmon_plugin_free_list"));
    auto ctrl_fn = reinterpret_cast<hmon_plugin_control_fn>(dlsym(handle, "hmon_plugin_control"));
    if (!name_fn || !init_fn || !collect_fn || !destroy_fn || !free_list_fn) {
        std::cerr << "[hmon] " << so_path << ": missing symbols\n"; dlclose(handle); return -1;
    }
    Plugin p;
    p.name = name_fn(); p.path = so_path; p.dl_handle = handle;
    p.ctx = nullptr; p.init = init_fn; p.collect = collect_fn;
    p.destroy = destroy_fn; p.free_list = free_list_fn; p.control_fn = ctrl_fn;
    plugins_.push_back(std::move(p));
    return 0;
}

int PluginManager::load_directory(const std::string& dir) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) return 0;
    int loaded = 0;
    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 4 || name.substr(name.size() - 3) != ".so") continue;
        if (name[0] == '.') continue;
        if (name == "libhmoncore.so" || name.rfind("libhmoncore.so.", 0) == 0) continue;
        if (load(dir + "/" + name) == 0) ++loaded;
    }
    closedir(dp);
    return loaded;
}

int PluginManager::init_all() {
    int failures = 0;
    for (auto& plugin : plugins_) {
        if (plugin.ctx) continue;
        int rc = plugin.init(&plugin.ctx);
        if (rc != 0) { std::cerr << "[hmon] plugin \"" << plugin.name << "\" init failed\n"; ++failures; }
    }
    return failures;
}

int PluginManager::collect_all() {
    kv_map_.clear();

    struct CollectResult {
        std::vector<std::pair<std::string, hmon_metric_value>> metrics;
        std::string plugin_name;
        bool success = false;
    };

    std::vector<std::future<CollectResult>> futures;
    futures.reserve(plugins_.size());

    for (auto& plugin : plugins_) {
        futures.push_back(std::async(std::launch::async, [&plugin]() -> CollectResult {
            CollectResult res;
            res.plugin_name = plugin.name;
            if (!plugin.ctx) return res;
            hmon_metric_list list{};
            int rc = plugin.collect(plugin.ctx, &list);
            if (rc != 0) return res;
            res.metrics.reserve(list.count);
            for (size_t i = 0; i < list.count; ++i) {
                res.metrics.emplace_back(list.items[i].key, list.items[i].value);
            }
            res.success = true;
            return res;
        }));
    }

    for (auto& fut : futures) {
        auto res = fut.get();
        if (res.success) {
            for (auto& [k, v] : res.metrics) {
                kv_map_.emplace(std::move(k), v);
            }
        }
    }
    return 0;
}

void PluginManager::destroy_all() {
    for (auto& plugin : plugins_) {
        if (plugin.ctx) { plugin.destroy(plugin.ctx); plugin.ctx = nullptr; }
        if (plugin.dl_handle) { dlclose(plugin.dl_handle); plugin.dl_handle = nullptr; }
    }
    plugins_.clear();
    kv_map_.clear();
}

std::string PluginManager::get_string(const std::string& key, const std::string& fallback) const {
    auto it = kv_map_.find(key);
    if (it != kv_map_.end() && it->second.type == HMON_VAL_STRING)
        return it->second.v.str ? it->second.v.str : fallback;
    return fallback;
}

std::optional<int64_t> PluginManager::get_int64(const std::string& key) const {
    auto it = kv_map_.find(key);
    if (it != kv_map_.end() && it->second.type == HMON_VAL_INT64) return it->second.v.i64;
    return std::nullopt;
}

std::optional<double> PluginManager::get_double(const std::string& key) const {
    auto it = kv_map_.find(key);
    if (it != kv_map_.end() && it->second.type == HMON_VAL_DOUBLE) return it->second.v.f64;
    return std::nullopt;
}

std::optional<bool> PluginManager::get_bool(const std::string& key) const {
    auto it = kv_map_.find(key);
    if (it != kv_map_.end() && it->second.type == HMON_VAL_BOOL) return it->second.v.b != 0;
    return std::nullopt;
}

std::vector<PluginManager::MetricEntry> PluginManager::get_by_prefix(const std::string& prefix) const {
    std::vector<MetricEntry> result;
    std::vector<std::pair<std::string, hmon_metric_value>> sorted;
    for (const auto& [k, v] : kv_map_) {
        if (k.rfind(prefix, 0) == 0) {
            sorted.emplace_back(k, v);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    result.reserve(sorted.size());
    for (const auto& [k, v] : sorted) {
        result.push_back({k, v});
    }
    return result;
}

std::vector<std::string> PluginManager::plugin_names() const {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& p : plugins_) names.push_back(p.name);
    return names;
}

void PluginManager::control(const std::string& plugin_name, const char* key, int value) {
    for (auto& plugin : plugins_)
        if (plugin.name == plugin_name && plugin.control_fn) { plugin.control_fn(key, value); return; }
}

}
