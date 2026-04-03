#pragma once

#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hmon/plugin_abi.h"

namespace hmon::core {

/* Direct function pointers for statically-linked plugins. */
struct StaticPlugin {
    const char* name;
    int  (*init)(hmon_plugin_ctx**);
    int  (*collect)(hmon_plugin_ctx*, hmon_metric_list*);
    void (*destroy)(hmon_plugin_ctx*);
    void (*free_list)(hmon_metric_list*);
    void (*control)(const char* key, int value);
};

std::vector<StaticPlugin>& staticPlugins();

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    int load_static();
    int load(const std::string& so_path);
    int load_directory(const std::string& dir);
    int init_all();
    int collect_all();
    void destroy_all();

    std::string get_string(const std::string& key, const std::string& fallback = "") const;
    std::optional<int64_t>  get_int64(const std::string& key) const;
    std::optional<double>   get_double(const std::string& key) const;
    std::optional<bool>     get_bool(const std::string& key) const;

    struct MetricEntry {
        std::string key;
        hmon_metric_value value;
    };
    std::vector<MetricEntry> get_by_prefix(const std::string& prefix) const;

    size_t plugin_count() const { return plugins_.size(); }
    std::vector<std::string> plugin_names() const;
    void control(const std::string& plugin_name, const char* key, int value);

private:
    struct Plugin {
        std::string  name;
        std::string  path;
        void*        dl_handle;
        hmon_plugin_ctx*               ctx;
        hmon_plugin_init_fn            init;
        hmon_plugin_collect_fn         collect;
        hmon_plugin_destroy_fn         destroy;
        hmon_plugin_free_list_fn       free_list;
        void                           (*control_fn)(const char*, int);
    };

    std::vector<Plugin> plugins_;
    std::unordered_map<std::string, hmon_metric_value> kv_map_;
};

} /* namespace hmon::core */
