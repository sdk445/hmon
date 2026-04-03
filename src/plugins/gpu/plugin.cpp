#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "gpu_collector.hpp"


struct GpuContext {};

static int gpu_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) GpuContext();
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

static int gpu_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    (void)ctx;
    if (!out_list) return -1;
    auto gpus = hmon::plugins::gpu::collectGpus();
    for (size_t i = 0; i < gpus.size(); ++i) {
        const auto& g = gpus[i];
        char key[128];
        std::snprintf(key, sizeof(key), "gpu.%zu.name", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, g.name.c_str());
        std::snprintf(key, sizeof(key), "gpu.%zu.source", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, g.source.c_str());
        if (g.temperature_c) {
            std::snprintf(key, sizeof(key), "gpu.%zu.temp_c", i);
            double v = *g.temperature_c;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.core_clock_mhz) {
            std::snprintf(key, sizeof(key), "gpu.%zu.clock_mhz", i);
            double v = *g.core_clock_mhz;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.utilization_percent) {
            std::snprintf(key, sizeof(key), "gpu.%zu.usage_pct", i);
            double v = *g.utilization_percent;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.power_w) {
            std::snprintf(key, sizeof(key), "gpu.%zu.power_w", i);
            double v = *g.power_w;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.memory_used_mib) {
            std::snprintf(key, sizeof(key), "gpu.%zu.vram_used_mib", i);
            double v = *g.memory_used_mib;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.memory_total_mib) {
            std::snprintf(key, sizeof(key), "gpu.%zu.vram_total_mib", i);
            double v = *g.memory_total_mib;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.memory_utilization_percent) {
            std::snprintf(key, sizeof(key), "gpu.%zu.vram_usage_pct", i);
            double v = *g.memory_utilization_percent;
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
        if (g.in_use.has_value()) {
            std::snprintf(key, sizeof(key), "gpu.%zu.in_use", i);
            int32_t v = *g.in_use ? 1 : 0;
            appendMetric(out_list, strdup(key), HMON_VAL_BOOL, &v);
        }
        for (size_t c = 0; c < g.gpu_core_usage_percent.size(); ++c) {
            std::snprintf(key, sizeof(key), "gpu.%zu.core_usage.%zu", i, c);
            double v = g.gpu_core_usage_percent[c];
            appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &v);
        }
    }
    return 0;
}

static void gpu_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    delete reinterpret_cast<GpuContext*>(ctx);
}

static void gpu_plugin_free_list(hmon_metric_list* list) {
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

HMON_STATIC_PLUGIN("gpu", gpu_plugin_init, gpu_plugin_collect, gpu_plugin_destroy, gpu_plugin_free_list, nullptr)
