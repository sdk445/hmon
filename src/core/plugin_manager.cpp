#include "hmon/plugin_manager.hpp"

#include <dlfcn.h>
#include <dirent.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace hmon::core {

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    destroy_all();
}

int PluginManager::load(const std::string& so_path) {
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "[hmon] dlopen(" << so_path << "): " << dlerror() << "\n";
        return -1;
    }

    /* Verify ABI version. */
    auto abi_fn = reinterpret_cast<hmon_plugin_abi_version_fn>(dlsym(handle, "hmon_plugin_abi_version"));
    if (!abi_fn) {
        std::cerr << "[hmon] " << so_path << ": missing hmon_plugin_abi_version\n";
        dlclose(handle);
        return -1;
    }
    if (abi_fn() != HMON_PLUGIN_ABI_VERSION) {
        std::cerr << "[hmon] " << so_path << ": ABI version mismatch (expected "
                  << HMON_PLUGIN_ABI_VERSION << ", got " << abi_fn() << ")\n";
        dlclose(handle);
        return -1;
    }

    /* Resolve required symbols. */
    auto name_fn = reinterpret_cast<hmon_plugin_name_fn>(dlsym(handle, "hmon_plugin_name"));
    auto init_fn = reinterpret_cast<hmon_plugin_init_fn>(dlsym(handle, "hmon_plugin_init"));
    auto collect_fn = reinterpret_cast<hmon_plugin_collect_fn>(dlsym(handle, "hmon_plugin_collect"));
    auto destroy_fn = reinterpret_cast<hmon_plugin_destroy_fn>(dlsym(handle, "hmon_plugin_destroy"));
    auto free_list_fn = reinterpret_cast<hmon_plugin_free_list_fn>(dlsym(handle, "hmon_plugin_free_list"));

    if (!name_fn || !init_fn || !collect_fn || !destroy_fn || !free_list_fn) {
        std::cerr << "[hmon] " << so_path << ": missing required plugin symbols\n";
        dlclose(handle);
        return -1;
    }

    Plugin p;
    p.name = name_fn();
    p.path = so_path;
    p.dl_handle = handle;
    p.ctx = nullptr;
    p.init = init_fn;
    p.collect = collect_fn;
    p.destroy = destroy_fn;
    p.free_list = free_list_fn;

    plugins_.push_back(std::move(p));
    return 0;
}

int PluginManager::load_directory(const std::string& dir) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) {
        return 0;
    }

    int loaded = 0;
    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        /* Only load .so files. */
        if (name.size() < 4 || name.substr(name.size() - 3) != ".so") {
            continue;
        }
        /* Skip hidden files. */
        if (name[0] == '.') {
            continue;
        }
        /* Skip the core library itself. */
        if (name == "libhmoncore.so" || name.rfind("libhmoncore.so.", 0) == 0) {
            continue;
        }

        std::string full_path = dir + "/" + name;
        if (load(full_path) == 0) {
            ++loaded;
        }
    }
    closedir(dp);
    return loaded;
}

int PluginManager::init_all() {
    int failures = 0;
    for (auto& plugin : plugins_) {
        if (plugin.ctx) {
            continue; /* Already initialised. */
        }
        int rc = plugin.init(&plugin.ctx);
        if (rc != 0) {
            std::cerr << "[hmon] plugin \"" << plugin.name << "\" init failed (rc=" << rc << ")\n";
            ++failures;
        }
    }
    return failures;
}

int PluginManager::collect_all() {
    kv_map_.clear();

    for (auto& plugin : plugins_) {
        if (!plugin.ctx) {
            continue;
        }

        hmon_metric_list list{};
        int rc = plugin.collect(plugin.ctx, &list);
        if (rc != 0) {
            std::cerr << "[hmon] plugin \"" << plugin.name << "\" collect failed (rc=" << rc << ")\n";
            continue;
        }

        for (size_t i = 0; i < list.count; ++i) {
            KeyValue kv;
            kv.key = list.items[i].key;
            kv.value = list.items[i].value;
            kv_map_.push_back(std::move(kv));
        }

        /* We do NOT free the list here — the plugin retains ownership of the
           string pointers until the next collect or destroy. */
    }

    return 0;
}

void PluginManager::destroy_all() {
    for (auto& plugin : plugins_) {
        if (plugin.ctx) {
            plugin.destroy(plugin.ctx);
            plugin.ctx = nullptr;
        }
        if (plugin.dl_handle) {
            dlclose(plugin.dl_handle);
            plugin.dl_handle = nullptr;
        }
    }
    plugins_.clear();
    kv_map_.clear();
}

std::string PluginManager::get_string(const std::string& key, const std::string& fallback) const {
    for (const auto& kv : kv_map_) {
        if (kv.key == key && kv.value.type == HMON_VAL_STRING) {
            return kv.value.v.str ? kv.value.v.str : fallback;
        }
    }
    return fallback;
}

std::optional<int64_t> PluginManager::get_int64(const std::string& key) const {
    for (const auto& kv : kv_map_) {
        if (kv.key == key && kv.value.type == HMON_VAL_INT64) {
            return kv.value.v.i64;
        }
    }
    return std::nullopt;
}

std::optional<double> PluginManager::get_double(const std::string& key) const {
    for (const auto& kv : kv_map_) {
        if (kv.key == key && kv.value.type == HMON_VAL_DOUBLE) {
            return kv.value.v.f64;
        }
    }
    return std::nullopt;
}

std::optional<bool> PluginManager::get_bool(const std::string& key) const {
    for (const auto& kv : kv_map_) {
        if (kv.key == key && kv.value.type == HMON_VAL_BOOL) {
            return kv.value.v.b != 0;
        }
    }
    return std::nullopt;
}

std::vector<PluginManager::MetricEntry> PluginManager::get_by_prefix(const std::string& prefix) const {
    std::vector<MetricEntry> result;
    for (const auto& kv : kv_map_) {
        if (kv.key.rfind(prefix, 0) == 0) {
            result.push_back({kv.key, kv.value});
        }
    }
    return result;
}

std::vector<std::string> PluginManager::plugin_names() const {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& p : plugins_) {
        names.push_back(p.name);
    }
    return names;
}

void PluginManager::control(const std::string& plugin_name, const char* key, int value) {
    for (auto& plugin : plugins_) {
        if (plugin.name == plugin_name) {
            auto ctrl_fn = reinterpret_cast<hmon_plugin_control_fn>(dlsym(plugin.dl_handle, "hmon_plugin_control"));
            if (ctrl_fn) {
                ctrl_fn(key, value);
            }
            return;
        }
    }
}

} /* namespace hmon::core */
