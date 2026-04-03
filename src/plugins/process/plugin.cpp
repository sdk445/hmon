#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "process_collector.hpp"

HMON_DECLARE_PLUGIN("process")

extern "C" {

HMON_PLUGIN_EXPORT int hmon_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) hmon::plugins::process::ProcessPluginCtx();
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
    case HMON_VAL_STRING: {
        const char* src = static_cast<const char*>(value);
        char* dup = strdup(src ? src : "");
        item->value.v.str = dup;
        break;
    }
    case HMON_VAL_INT64:
        item->value.v.i64 = *static_cast<const int64_t*>(value);
        break;
    case HMON_VAL_DOUBLE:
        item->value.v.f64 = *static_cast<const double*>(value);
        break;
    case HMON_VAL_BOOL:
        item->value.v.b = *static_cast<const int32_t*>(value);
        break;
    }
    ++list->count;
}

HMON_PLUGIN_EXPORT int hmon_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;

    auto* c = reinterpret_cast<hmon::plugins::process::ProcessPluginCtx*>(ctx);

    size_t limit = 20;
    auto procs = hmon::plugins::process::collectTopProcesses(limit, c->sort_mode, c->lock_pid);

    for (size_t i = 0; i < procs.size(); ++i) {
        const auto& p = procs[i];
        char key[128];

        std::snprintf(key, sizeof(key), "proc.%zu.pid", i);
        int64_t pid = p.pid;
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &pid);

        std::snprintf(key, sizeof(key), "proc.%zu.cpu_pct", i);
        double cpu = p.cpu_percent;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &cpu);

        std::snprintf(key, sizeof(key), "proc.%zu.mem_pct", i);
        double mem = p.mem_percent;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &mem);

        std::snprintf(key, sizeof(key), "proc.%zu.gpu_pct", i);
        double gpu = p.gpu_percent;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &gpu);

        std::snprintf(key, sizeof(key), "proc.%zu.command", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, p.command.c_str());
    }

    return 0;
}

HMON_PLUGIN_EXPORT void hmon_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    auto* c = reinterpret_cast<hmon::plugins::process::ProcessPluginCtx*>(ctx);
    delete c;
}

HMON_PLUGIN_EXPORT void hmon_plugin_free_list(hmon_metric_list* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].value.type == HMON_VAL_STRING && list->items[i].value.v.str) {
            free(const_cast<char*>(list->items[i].value.v.str));
        }
        free(const_cast<char*>(list->items[i].key));
    }
    free(list->items);
    list->items = nullptr;
    list->count = 0;
    list->capacity = 0;
}

} /* extern "C" */
