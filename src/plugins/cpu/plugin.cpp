#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "cpu_collector.hpp"


struct CpuContext {
    hmon::plugins::cpu::CpuPluginCtx collector;
};

static int cpu_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) CpuContext();
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

static int cpu_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;
    auto* c = reinterpret_cast<CpuContext*>(ctx);

    std::string name = hmon::plugins::cpu::collectName();
    int64_t cores = 0, threads = 0;
    double temp = 0.0, freq = 0.0, usage = 0.0;
    bool has_cores = false, has_threads = false, has_temp = false, has_freq = false, has_usage = false;

    auto tc = hmon::plugins::cpu::collectThreadCount();
    if (tc) { threads = *tc; has_threads = true; }
    auto cc = hmon::plugins::cpu::collectCoreCount();
    if (cc) { cores = *cc; has_cores = true; }
    auto t = hmon::plugins::cpu::collectTemperature();
    if (t) { temp = *t; has_temp = true; }
    auto f = hmon::plugins::cpu::collectFrequency();
    if (f) { freq = *f; has_freq = true; }
    auto u = hmon::plugins::cpu::collectUsagePercent(&c->collector);
    if (u) { usage = *u; has_usage = true; }
    auto per_core = hmon::plugins::cpu::collectPerCoreUsagePercent(&c->collector);

    appendMetric(out_list, HMON_METRIC_CPU_NAME, HMON_VAL_STRING, name.c_str());
    if (has_cores)  appendMetric(out_list, HMON_METRIC_CPU_CORES, HMON_VAL_INT64, &cores);
    if (has_threads) appendMetric(out_list, HMON_METRIC_CPU_THREADS, HMON_VAL_INT64, &threads);
    if (has_temp)   appendMetric(out_list, HMON_METRIC_CPU_TEMP_C, HMON_VAL_DOUBLE, &temp);
    if (has_freq)   appendMetric(out_list, HMON_METRIC_CPU_FREQ_MHZ, HMON_VAL_DOUBLE, &freq);
    if (has_usage)  appendMetric(out_list, HMON_METRIC_CPU_USAGE_PCT, HMON_VAL_DOUBLE, &usage);

    for (size_t i = 0; i < per_core.size(); ++i) {
        char key[64];
        std::snprintf(key, sizeof(key), "%s.%zu", HMON_METRIC_CPU_CORE_USAGE_PCT, i);
        double val = per_core[i];
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &val);
    }
    return 0;
}

static void cpu_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    delete reinterpret_cast<CpuContext*>(ctx);
}

static void cpu_plugin_free_list(hmon_metric_list* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].value.type == HMON_VAL_STRING && list->items[i].value.v.str) {
            if (std::strncmp(list->items[i].key, "cpu.core_usage_pct", 18) == 0)
                free(const_cast<char*>(list->items[i].key));
            free(const_cast<char*>(list->items[i].value.v.str));
        }
    }
    free(list->items);
    list->items = nullptr;
    list->count = 0;
    list->capacity = 0;
}

HMON_STATIC_PLUGIN("cpu", cpu_plugin_init, cpu_plugin_collect, cpu_plugin_destroy, cpu_plugin_free_list, nullptr)
