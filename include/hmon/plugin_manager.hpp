#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"

namespace hmon::core {

/* ── Plugin descriptor (internal) ────────────────────────────────────────── */

struct Plugin {
    std::string  name;
    std::string  path;
    void*        dl_handle;

    hmon_plugin_ctx*               ctx;
    hmon_plugin_init_fn            init;
    hmon_plugin_collect_fn         collect;
    hmon_plugin_destroy_fn         destroy;
    hmon_plugin_free_list_fn       free_list;
};

/* ── PluginManager ───────────────────────────────────────────────────────── */

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    /* Load a plugin from an absolute .so path.  Returns 0 on success. */
    int load(const std::string& so_path);

    /* Load all .so files found in a directory.  Returns count loaded. */
    int load_directory(const std::string& dir);

    /* Initialise every loaded plugin.  Returns 0 if all succeed. */
    int init_all();

    /* Collect metrics from every plugin.  Results are stored internally
       and can be retrieved via the typed accessors below. */
    int collect_all();

    /* Tear down every plugin and release handles. */
    void destroy_all();

    /* ── Metric accessors (convenience wrappers over raw metric lists) ──── */

    std::string get_string(const std::string& key, const std::string& fallback = "") const;
    std::optional<int64_t>  get_int64(const std::string& key) const;
    std::optional<double>   get_double(const std::string& key) const;
    std::optional<bool>     get_bool(const std::string& key) const;

    /* Return all metric values whose key starts with `prefix`. */
    struct MetricEntry {
        std::string key;
        hmon_metric_value value;
    };
    std::vector<MetricEntry> get_by_prefix(const std::string& prefix) const;

    /* Return the number of loaded plugins. */
    size_t plugin_count() const { return plugins_.size(); }

    /* Return plugin names. */
    std::vector<std::string> plugin_names() const;

private:
    std::vector<Plugin> plugins_;

    /* Flattened key→value map built during collect_all(). */
    struct KeyValue {
        std::string key;
        hmon_metric_value value;
        /* We do NOT own the string pointers — they point into plugin-allocated
           metric lists that live until the next collect_all() or destroy_all(). */
    };
    std::vector<KeyValue> kv_map_;
};

} /* namespace hmon::core */
