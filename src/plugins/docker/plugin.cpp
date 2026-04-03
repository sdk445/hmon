#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hmon/plugin_abi.h"
#include "hmon/static_plugins.hpp"
#include "docker_collector.hpp"


static hmon::plugins::docker::DockerPluginCtx* g_docker_ctx = nullptr;

static void docker_plugin_control(const char* key, int value);

static int docker_plugin_init(hmon_plugin_ctx** out) {
    if (!out) return -1;
    auto* ctx = new (std::nothrow) hmon::plugins::docker::DockerPluginCtx();
    if (!ctx) return -1;
    const char* env_socket = std::getenv("DOCKER_HOST");
    if (env_socket && std::strncmp(env_socket, "unix://", 7) == 0) {
        ctx->socket_path = env_socket + 7;
    } else {
        ctx->socket_path = "/var/run/docker.sock";
    }
    g_docker_ctx = ctx;
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

static int docker_plugin_collect(hmon_plugin_ctx* ctx, hmon_metric_list* out_list) {
    if (!ctx || !out_list) return -1;
    auto* c = reinterpret_cast<hmon::plugins::docker::DockerPluginCtx*>(ctx);
    std::vector<hmon::plugins::docker::ContainerStats> containers;
    {
        std::lock_guard<std::mutex> lock(c->data_mutex);
        containers = c->containers;
    }
    for (size_t i = 0; i < containers.size(); ++i) {
        const auto& ct = containers[i];
        char key[256];
        std::snprintf(key, sizeof(key), "docker.%zu.name", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, ct.name.c_str());
        std::snprintf(key, sizeof(key), "docker.%zu.image", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, ct.image.c_str());
        std::snprintf(key, sizeof(key), "docker.%zu.state", i);
        appendMetric(out_list, strdup(key), HMON_VAL_STRING, ct.state.c_str());
        std::snprintf(key, sizeof(key), "docker.%zu.cpu_pct", i);
        double cpu = ct.cpu_percent;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &cpu);
        std::snprintf(key, sizeof(key), "docker.%zu.mem_usage", i);
        int64_t mem = static_cast<int64_t>(ct.mem_usage);
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &mem);
        std::snprintf(key, sizeof(key), "docker.%zu.mem_limit", i);
        int64_t mem_lim = static_cast<int64_t>(ct.mem_limit);
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &mem_lim);
        std::snprintf(key, sizeof(key), "docker.%zu.mem_pct", i);
        double mem_p = ct.mem_percent;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &mem_p);
        std::snprintf(key, sizeof(key), "docker.%zu.net_rx_bps", i);
        double rx = ct.net_rx_bps;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &rx);
        std::snprintf(key, sizeof(key), "docker.%zu.net_tx_bps", i);
        double tx = ct.net_tx_bps;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &tx);
        std::snprintf(key, sizeof(key), "docker.%zu.net_rx_total", i);
        int64_t rx_total = static_cast<int64_t>(ct.net_rx_total);
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &rx_total);
        std::snprintf(key, sizeof(key), "docker.%zu.net_tx_total", i);
        int64_t tx_total = static_cast<int64_t>(ct.net_tx_total);
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &tx_total);
        std::snprintf(key, sizeof(key), "docker.%zu.blk_read_bps", i);
        double br = ct.blk_read_bps;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &br);
        std::snprintf(key, sizeof(key), "docker.%zu.blk_write_bps", i);
        double bw = ct.blk_write_bps;
        appendMetric(out_list, strdup(key), HMON_VAL_DOUBLE, &bw);
        std::snprintf(key, sizeof(key), "docker.%zu.pids", i);
        int64_t pids = ct.pids_current;
        appendMetric(out_list, strdup(key), HMON_VAL_INT64, &pids);
    }
    return 0;
}

static void docker_plugin_destroy(hmon_plugin_ctx* ctx) {
    if (!ctx) return;
    auto* c = reinterpret_cast<hmon::plugins::docker::DockerPluginCtx*>(ctx);
    hmon::plugins::docker::stopBackgroundCollector(c);
    g_docker_ctx = nullptr;
    delete c;
}

static void docker_plugin_free_list(hmon_metric_list* list) {
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

static void docker_plugin_control(const char* key, int value) {
    if (!g_docker_ctx) return;
    if (std::string(key) == "docker.enable") {
        if (value) hmon::plugins::docker::startBackgroundCollector(g_docker_ctx);
        else hmon::plugins::docker::stopBackgroundCollector(g_docker_ctx);
    }
}

HMON_STATIC_PLUGIN("docker", docker_plugin_init, docker_plugin_collect, docker_plugin_destroy, docker_plugin_free_list, docker_plugin_control)
