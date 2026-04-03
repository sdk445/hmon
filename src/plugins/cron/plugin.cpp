#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "cron_collector.hpp"


extern "C" {
    int cron_plugin_init(hmon_plugin_ctx**);
    int cron_plugin_collect(hmon_plugin_ctx*, hmon_metric_list*);
    void cron_plugin_destroy(hmon_plugin_ctx*);
    void cron_plugin_free_list(hmon_metric_list*);
}

HMON_STATIC_PLUGIN("cron", cron_plugin_init, cron_plugin_collect, cron_plugin_destroy, cron_plugin_free_list, nullptr)

extern "C" {

HMON_PLUGIN_EXPORT int cron_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) hmon::plugins::cron::CronPluginCtx();
    if (!ctx) return -1;
    *out = reinterpret_cast<hmon_plugin_ctx*>(ctx);
    return 0;
}

static void appendMetric(hmon_metric_list* list, const char* key, int type, const void* value) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 32 : list->capacity * 2;
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

HMON_PLUGIN_EXPORT int cron_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;
    auto* c = reinterpret_cast<hmon::plugins::cron::CronPluginCtx*>(ctx);
    auto jobs = hmon::plugins::cron::collectCronJobs(c);
    for (size_t i = 0; i < jobs.size(); ++i) {
        const auto& j = jobs[i];
        char key[256];
        std::snprintf(key, sizeof(key), "cron.%zu.schedule", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, j.schedule.c_str());
        std::snprintf(key, sizeof(key), "cron.%zu.user", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, j.user.c_str());
        std::snprintf(key, sizeof(key), "cron.%zu.command", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, j.command.c_str());
        std::snprintf(key, sizeof(key), "cron.%zu.source", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, j.source.c_str());
    }
    return 0;
}

HMON_PLUGIN_EXPORT void cron_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    delete reinterpret_cast<hmon::plugins::cron::CronPluginCtx*>(ctx);
}

HMON_PLUGIN_EXPORT void cron_plugin_free_list(hmon_metric_list* list) {
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
