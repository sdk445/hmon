#include <cstdlib>
#include <cstring>
#include <string>

#include "hmon/plugin_abi.h"
#include "system_collector.hpp"

HMON_DECLARE_PLUGIN("system")

extern "C" {

HMON_PLUGIN_EXPORT int hmon_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) hmon::plugins::system::SystemPluginCtx();
    if (!ctx) return -1;
    ctx->root_device = hmon::plugins::system::detectRootDevice();
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

HMON_PLUGIN_EXPORT int hmon_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;

    auto* c = reinterpret_cast<hmon::plugins::system::SystemPluginCtx*>(ctx);

    /* RAM */
    auto ram_total = hmon::plugins::system::collectRamTotalKb();
    auto ram_avail = hmon::plugins::system::collectRamAvailableKb();
    if (ram_total) {
        int64_t v = *ram_total;
        appendMetric(out_list, HMON_METRIC_RAM_TOTAL_KB, HMON_VAL_INT64, &v);
    }
    if (ram_avail) {
        int64_t v = *ram_avail;
        appendMetric(out_list, HMON_METRIC_RAM_AVAILABLE_KB, HMON_VAL_INT64, &v);
    }

    /* Disk */
    auto disk_total = hmon::plugins::system::collectDiskTotalBytes("/");
    auto disk_free = hmon::plugins::system::collectDiskFreeBytes("/");
    appendMetric(out_list, HMON_METRIC_DISK_MOUNT, HMON_VAL_STRING, "/");
    if (disk_total) {
        int64_t v = static_cast<int64_t>(*disk_total);
        appendMetric(out_list, HMON_METRIC_DISK_TOTAL_BYTES, HMON_VAL_INT64, &v);
    }
    if (disk_free) {
        int64_t v = static_cast<int64_t>(*disk_free);
        appendMetric(out_list, HMON_METRIC_DISK_FREE_BYTES, HMON_VAL_INT64, &v);
    }

    /* Network */
    auto rx = hmon::plugins::system::collectRxKbps(c);
    auto tx = hmon::plugins::system::collectTxKbps(c);
    if (!c->active_interface.empty()) {
        appendMetric(out_list, HMON_METRIC_NET_INTERFACE, HMON_VAL_STRING, c->active_interface.c_str());
    }
    if (rx) {
        double v = *rx;
        appendMetric(out_list, HMON_METRIC_NET_RX_KBPS, HMON_VAL_DOUBLE, &v);
    }
    if (tx) {
        double v = *tx;
        appendMetric(out_list, HMON_METRIC_NET_TX_KBPS, HMON_VAL_DOUBLE, &v);
    }

    /* Swap */
    auto swap_total = hmon::plugins::system::getSwapTotalKb();
    auto swap_free = hmon::plugins::system::getSwapFreeKb();
    if (swap_total) {
        int64_t v = *swap_total;
        appendMetric(out_list, "swap.total_kb", HMON_VAL_INT64, &v);
    }
    if (swap_free) {
        int64_t v = *swap_free;
        appendMetric(out_list, "swap.free_kb", HMON_VAL_INT64, &v);
    }

    return 0;
}

HMON_PLUGIN_EXPORT void hmon_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    auto* c = reinterpret_cast<hmon::plugins::system::SystemPluginCtx*>(ctx);
    delete c;
}

HMON_PLUGIN_EXPORT void hmon_plugin_free_list(hmon_metric_list* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].value.type == HMON_VAL_STRING && list->items[i].value.v.str) {
            free(const_cast<char*>(list->items[i].value.v.str));
        }
    }
    free(list->items);
    list->items = nullptr;
    list->count = 0;
    list->capacity = 0;
}

} /* extern "C" */
