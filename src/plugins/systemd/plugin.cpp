#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "systemd_collector.hpp"


extern "C" {
    int systemd_plugin_init(hmon_plugin_ctx**);
    int systemd_plugin_collect(hmon_plugin_ctx*, hmon_metric_list*);
    void systemd_plugin_destroy(hmon_plugin_ctx*);
    void systemd_plugin_free_list(hmon_metric_list*);
}

HMON_STATIC_PLUGIN("systemd", systemd_plugin_init, systemd_plugin_collect, systemd_plugin_destroy, systemd_plugin_free_list, nullptr)

extern "C" {

HMON_PLUGIN_EXPORT int systemd_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) hmon::plugins::systemd::SystemdPluginCtx();
    if (!ctx) return -1;
    *out = reinterpret_cast<hmon_plugin_ctx*>(ctx);
    return 0;
}

static void appendMetric(hmon_metric_list* list, const char* key, int type, const void* value) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 64 : list->capacity * 2;
        auto* new_items = static_cast<hmon_metric*>(std::realloc(list->items, new_cap * sizeof(hmon_metric)));
        if (!new_items) return;
        list->items = new_items;
        list->capacity = new_cap;
    }
    auto* item = &list->items[list->count];
    item->key = key;
    item->value.type = type;
    switch (type) {
    case HMON_VAL_STRING: item->value.v.str = strdup(static_cast<const char*>(value) ? static_cast<const char*>(value) : ""); break;
    case HMON_VAL_INT64: item->value.v.i64 = *static_cast<const int64_t*>(value); break;
    case HMON_VAL_DOUBLE: item->value.v.f64 = *static_cast<const double*>(value); break;
    case HMON_VAL_BOOL: item->value.v.b = *static_cast<const int32_t*>(value); break;
    }
    ++list->count;
}

HMON_PLUGIN_EXPORT int systemd_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;
    auto* c = reinterpret_cast<hmon::plugins::systemd::SystemdPluginCtx*>(ctx);
    auto services = hmon::plugins::systemd::collectServices(c);
    for (size_t i = 0; i < services.size(); ++i) {
        const auto& s = services[i];
        char key[256];
        std::snprintf(key, sizeof(key), "systemd.%zu.name", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, s.name.c_str());
        std::snprintf(key, sizeof(key), "systemd.%zu.state", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, s.active_state.c_str());
        std::snprintf(key, sizeof(key), "systemd.%zu.sub", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, s.sub_state.c_str());
        std::snprintf(key, sizeof(key), "systemd.%zu.desc", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, s.description.c_str());
    }
    return 0;
}

HMON_PLUGIN_EXPORT void systemd_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    delete reinterpret_cast<hmon::plugins::systemd::SystemdPluginCtx*>(ctx);
}

HMON_PLUGIN_EXPORT void systemd_plugin_free_list(hmon_metric_list* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].value.type == HMON_VAL_STRING && list->items[i].value.v.str)
            free(const_cast<char*>(list->items[i].value.v.str));
        free(const_cast<char*>(list->items[i].key));
    }
    free(list->items);
    list->items = nullptr;
    list->count = 0;
    list->capacity = 0;
}

}
