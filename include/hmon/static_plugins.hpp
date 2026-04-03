#pragma once

#include <vector>

#include "hmon/plugin_abi.h"

namespace hmon::core {

struct StaticPlugin {
    const char* name;
    int  (*init)(hmon_plugin_ctx**);
    int  (*collect)(hmon_plugin_ctx*, hmon_metric_list*);
    void (*destroy)(hmon_plugin_ctx*);
    void (*free_list)(hmon_metric_list*);
    void (*control)(const char* key, int value);
};

std::vector<StaticPlugin>& staticPlugins();

}

/* Register a statically-linked plugin. Each plugin must have unique function names. */
#define HMON_STATIC_PLUGIN(name_str, init_fn, collect_fn, destroy_fn, free_fn, ctrl_fn) \
    namespace { struct _hmon_reg { _hmon_reg() { \
        hmon::core::StaticPlugin sp; \
        sp.name = name_str; sp.init = init_fn; sp.collect = collect_fn; \
        sp.destroy = destroy_fn; sp.free_list = free_fn; sp.control = ctrl_fn; \
        hmon::core::staticPlugins().push_back(sp); \
    }} _hmon_reg_instance; }
