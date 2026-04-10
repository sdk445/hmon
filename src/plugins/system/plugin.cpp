#include <cstdlib>
#include <cstring>
#include <string>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "system_collector.hpp"


static int system_plugin_init(hmon_plugin_ctx** out) {
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
    item->key = strdup(key ? key : "");
    item->value.type = type;
    switch (type) {
    case HMON_VAL_STRING: item->value.v.str = strdup(static_cast<const char*>(value) ? static_cast<const char*>(value) : ""); break;
    case HMON_VAL_INT64: item->value.v.i64 = *static_cast<const int64_t*>(value); break;
    case HMON_VAL_DOUBLE: item->value.v.f64 = *static_cast<const double*>(value); break;
    case HMON_VAL_BOOL: item->value.v.b = *static_cast<const int32_t*>(value); break;
    }
    ++list->count;
}

static int system_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;
    auto* c = reinterpret_cast<hmon::plugins::system::SystemPluginCtx*>(ctx);

    auto ram_total = hmon::plugins::system::collectRamTotalKb();
    auto ram_avail = hmon::plugins::system::collectRamAvailableKb();
    if (ram_total) { int64_t v = *ram_total; appendMetric(out_list, HMON_METRIC_RAM_TOTAL_KB, HMON_VAL_INT64, &v); }
    if (ram_avail) { int64_t v = *ram_avail; appendMetric(out_list, HMON_METRIC_RAM_AVAILABLE_KB, HMON_VAL_INT64, &v); }

    auto disk_total = hmon::plugins::system::collectDiskTotalBytes("/");
    auto disk_free = hmon::plugins::system::collectDiskFreeBytes("/");
    appendMetric(out_list, HMON_METRIC_DISK_MOUNT, HMON_VAL_STRING, "/");
    if (disk_total) { int64_t v = static_cast<int64_t>(*disk_total); appendMetric(out_list, HMON_METRIC_DISK_TOTAL_BYTES, HMON_VAL_INT64, &v); }
    if (disk_free) { int64_t v = static_cast<int64_t>(*disk_free); appendMetric(out_list, HMON_METRIC_DISK_FREE_BYTES, HMON_VAL_INT64, &v); }

    auto rx = hmon::plugins::system::collectRxKbps(c);
    auto tx = hmon::plugins::system::collectTxKbps(c);
    if (!c->active_interface.empty()) appendMetric(out_list, HMON_METRIC_NET_INTERFACE, HMON_VAL_STRING, c->active_interface.c_str());
    if (rx) { double v = *rx; appendMetric(out_list, HMON_METRIC_NET_RX_KBPS, HMON_VAL_DOUBLE, &v); }
    if (tx) { double v = *tx; appendMetric(out_list, HMON_METRIC_NET_TX_KBPS, HMON_VAL_DOUBLE, &v); }

    auto swap_total = hmon::plugins::system::getSwapTotalKb();
    auto swap_free = hmon::plugins::system::getSwapFreeKb();
    if (swap_total) { int64_t v = *swap_total; appendMetric(out_list, "swap.total_kb", HMON_VAL_INT64, &v); }
    if (swap_free) { int64_t v = *swap_free; appendMetric(out_list, "swap.free_kb", HMON_VAL_INT64, &v); }

    auto disk_io = hmon::plugins::system::collectDiskIoDelta(c);
    for (size_t i = 0; i < disk_io.size(); ++i) {
        std::string prefix = "diskio." + std::to_string(i) + ".";
        appendMetric(out_list, (prefix + "name").c_str(), HMON_VAL_STRING, disk_io[i].name.c_str());
        double rk = disk_io[i].read_kbps;
        double wk = disk_io[i].write_kbps;
        double bp = disk_io[i].busy_percent;
        uint64_t ro = disk_io[i].read_ops;
        uint64_t wo = disk_io[i].write_ops;
        appendMetric(out_list, (prefix + "read_kb").c_str(), HMON_VAL_DOUBLE, &rk);
        appendMetric(out_list, (prefix + "write_kb").c_str(), HMON_VAL_DOUBLE, &wk);
        appendMetric(out_list, (prefix + "busy_pct").c_str(), HMON_VAL_DOUBLE, &bp);
        int64_t r_ops = static_cast<int64_t>(ro);
        int64_t w_ops = static_cast<int64_t>(wo);
        appendMetric(out_list, (prefix + "read_ops").c_str(), HMON_VAL_INT64, &r_ops);
        appendMetric(out_list, (prefix + "write_ops").c_str(), HMON_VAL_INT64, &w_ops);
    }

    auto net_conn = hmon::plugins::system::collectNetConnStats();
    auto emit_conn = [&](const char* key, int val) {
        int64_t v = val;
        appendMetric(out_list, key, HMON_VAL_INT64, &v);
    };
    emit_conn("netconn.established", net_conn.established);
    emit_conn("netconn.syn_sent", net_conn.syn_sent);
    emit_conn("netconn.syn_recv", net_conn.syn_recv);
    emit_conn("netconn.fin_wait1", net_conn.fin_wait1);
    emit_conn("netconn.fin_wait2", net_conn.fin_wait2);
    emit_conn("netconn.time_wait", net_conn.time_wait);
    emit_conn("netconn.close", net_conn.close);
    emit_conn("netconn.close_wait", net_conn.close_wait);
    emit_conn("netconn.last_ack", net_conn.last_ack);
    emit_conn("netconn.listen", net_conn.listen);
    emit_conn("netconn.closing", net_conn.closing);

    auto mem = hmon::plugins::system::collectMemInfoDetailed();
    auto emit_mem = [&](const char* key, long long val) {
        int64_t v = val;
        appendMetric(out_list, key, HMON_VAL_INT64, &v);
    };
    emit_mem("meminfo.buffers_kb", mem.buffers_kb);
    emit_mem("meminfo.cached_kb", mem.cached_kb);
    emit_mem("meminfo.shared_kb", mem.shared_kb);
    emit_mem("meminfo.slab_kb", mem.slab_kb);
    emit_mem("meminfo.sreclaimable_kb", mem.sreclaimable_kb);
    emit_mem("meminfo.active_kb", mem.active_kb);
    emit_mem("meminfo.inactive_kb", mem.inactive_kb);
    emit_mem("meminfo.dirty_kb", mem.dirty_kb);
    emit_mem("meminfo.writeback_kb", mem.writeback_kb);
    emit_mem("meminfo.hugepages_total_kb", mem.hugepages_total_kb);
    emit_mem("meminfo.mapped_kb", mem.mapped_kb);
    emit_mem("meminfo.page_tables_kb", mem.page_tables_kb);
    emit_mem("meminfo.nfs_unstable_kb", mem.nfs_unstable_kb);
    emit_mem("meminfo.bounce_kb", mem.bounce_kb);

    auto sys = hmon::plugins::system::collectSystemStats();
    double la1 = sys.load_avg_1, la5 = sys.load_avg_5, la15 = sys.load_avg_15;
    int64_t pr = sys.procs_running, pb = sys.procs_blocked;
    int64_t ut = sys.uptime_seconds;
    int64_t cs = static_cast<int64_t>(sys.context_switches);
    int64_t ir = static_cast<int64_t>(sys.interrupts);
    int64_t si = static_cast<int64_t>(sys.softirqs);
    int64_t fk = static_cast<int64_t>(sys.forks);
    int64_t fo = static_cast<int64_t>(sys.fd_open);
    int64_t fm = static_cast<int64_t>(sys.fd_max);
    appendMetric(out_list, "sysstat.load_avg_1", HMON_VAL_DOUBLE, &la1);
    appendMetric(out_list, "sysstat.load_avg_5", HMON_VAL_DOUBLE, &la5);
    appendMetric(out_list, "sysstat.load_avg_15", HMON_VAL_DOUBLE, &la15);
    appendMetric(out_list, "sysstat.procs_running", HMON_VAL_INT64, &pr);
    appendMetric(out_list, "sysstat.procs_blocked", HMON_VAL_INT64, &pb);
    appendMetric(out_list, "sysstat.uptime_seconds", HMON_VAL_INT64, &ut);
    appendMetric(out_list, "sysstat.context_switches", HMON_VAL_INT64, &cs);
    appendMetric(out_list, "sysstat.interrupts", HMON_VAL_INT64, &ir);
    appendMetric(out_list, "sysstat.softirqs", HMON_VAL_INT64, &si);
    appendMetric(out_list, "sysstat.forks", HMON_VAL_INT64, &fk);
    appendMetric(out_list, "sysstat.fd_open", HMON_VAL_INT64, &fo);
    appendMetric(out_list, "sysstat.fd_max", HMON_VAL_INT64, &fm);

    return 0;
}

static void system_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    delete reinterpret_cast<hmon::plugins::system::SystemPluginCtx*>(ctx);
}

static void system_plugin_free_list(hmon_metric_list* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(const_cast<char*>(list->items[i].key));
        if (list->items[i].value.type == HMON_VAL_STRING && list->items[i].value.v.str)
            free(const_cast<char*>(list->items[i].value.v.str));
    }
    free(list->items);
    list->items = nullptr;
    list->count = 0;
    list->capacity = 0;
}

HMON_STATIC_PLUGIN("system", system_plugin_init, system_plugin_collect, system_plugin_destroy, system_plugin_free_list, nullptr)
