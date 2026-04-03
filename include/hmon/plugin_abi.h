/**
 * hmon plugin ABI — stable C interface for dynamically loaded metric collectors.
 *
 * Every plugin .so must export exactly the symbols documented below.  The host
 * (libhmoncore) discovers plugins at runtime via dlopen/dlsym and calls into
 * them through opaque handles.
 *
 * All strings returned by plugins are owned by the plugin and must be copied
 * by the host if they need to outlive the next collect() call.
 *
 * ABI version is bumped whenever the layout of any struct or the signature of
 * any exported symbol changes.  The host refuses to load a plugin whose
 * ABI_VERSION does not match HMON_PLUGIN_ABI_VERSION.
 */

#ifndef HMON_PLUGIN_ABI_H
#define HMON_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ─────────────────────────────────────────────────────────── */

#define HMON_PLUGIN_ABI_VERSION 1

/* ── Opaque handles ──────────────────────────────────────────────────────── */

typedef struct hmon_plugin_ctx  hmon_plugin_ctx;
typedef struct hmon_metric_list hmon_metric_list;

/* ── Metric value types ──────────────────────────────────────────────────── */

enum hmon_value_type {
    HMON_VAL_STRING   = 0,
    HMON_VAL_INT64    = 1,
    HMON_VAL_DOUBLE   = 2,
    HMON_VAL_BOOL     = 3,
};

/* A single metric value (discriminated union). */
struct hmon_metric_value {
    int32_t type;               /* one of hmon_value_type                     */
    union {
        const char* str;
        int64_t     i64;
        double      f64;
        int32_t     b;
    } v;
};

/* A named metric (e.g. "cpu.usage_percent", "ram.total_kb"). */
struct hmon_metric {
    const char*              key;   /* dot-separated hierarchical name        */
    struct hmon_metric_value value;
};

/* A growable list of metrics — owned by the plugin. */
struct hmon_metric_list {
    struct hmon_metric* items;
    size_t              count;
    size_t              capacity;
};

/* ── Plugin lifecycle ────────────────────────────────────────────────────── */

/**
 * Return the ABI version this plugin was compiled against.
 * Required symbol:  int hmon_plugin_abi_version(void);
 */
typedef int (*hmon_plugin_abi_version_fn)(void);

/**
 * Return a human-readable plugin name (static string).
 * Required symbol:  const char* hmon_plugin_name(void);
 */
typedef const char* (*hmon_plugin_name_fn)(void);

/**
 * Initialise the plugin.  Called once before any collect().
 * Required symbol:  int hmon_plugin_init(hmon_plugin_ctx** out);
 *
 * Returns 0 on success, non-zero on failure.
 * On success *out must point to a plugin-allocated context.
 */
typedef int (*hmon_plugin_init_fn)(hmon_plugin_ctx** out);

/**
 * Collect a fresh snapshot of metrics.
 * Required symbol:  int hmon_plugin_collect(hmon_plugin_ctx* ctx,
 *                                           hmon_metric_list* out_list);
 *
 * The host provides a pre-zeroed hmon_metric_list.  The plugin allocates
 * items[] with malloc() and populates count/capacity.
 * Returns 0 on success, non-zero on failure.
 */
typedef int (*hmon_plugin_collect_fn)(hmon_plugin_ctx* ctx,
                                      hmon_metric_list* out_list);

/**
 * Tear down the plugin context and free all resources.
 * Required symbol:  void hmon_plugin_destroy(hmon_plugin_ctx* ctx);
 */
typedef void (*hmon_plugin_destroy_fn)(hmon_plugin_ctx* ctx);

/**
 * Free a metric list previously returned by collect().
 * Required symbol:  void hmon_plugin_free_list(hmon_metric_list* list);
 */
typedef void (*hmon_plugin_free_list_fn)(hmon_metric_list* list);

/* ── Helper macros for plugins ───────────────────────────────────────────── */

#define HMON_PLUGIN_EXPORT __attribute__((visibility("default")))

#define HMON_DECLARE_PLUGIN(name)                                              \
    extern "C" {                                                                 \
    HMON_PLUGIN_EXPORT int hmon_plugin_abi_version(void)                       \
    {                                                                          \
        return HMON_PLUGIN_ABI_VERSION;                                        \
    }                                                                          \
    HMON_PLUGIN_EXPORT const char* hmon_plugin_name(void)                      \
    {                                                                          \
        return name;                                                           \
    }                                                                          \
    }

/* ── Metric key constants (conventions, not ABI-enforced) ────────────────── */

/* CPU */
#define HMON_METRIC_CPU_NAME              "cpu.name"
#define HMON_METRIC_CPU_CORES             "cpu.cores"
#define HMON_METRIC_CPU_THREADS           "cpu.threads"
#define HMON_METRIC_CPU_TEMP_C            "cpu.temp_c"
#define HMON_METRIC_CPU_FREQ_MHZ          "cpu.freq_mhz"
#define HMON_METRIC_CPU_USAGE_PCT         "cpu.usage_pct"
#define HMON_METRIC_CPU_CORE_USAGE_PCT    "cpu.core_usage_pct"  /* array index */

/* RAM */
#define HMON_METRIC_RAM_TOTAL_KB          "ram.total_kb"
#define HMON_METRIC_RAM_AVAILABLE_KB      "ram.available_kb"

/* DISK */
#define HMON_METRIC_DISK_MOUNT            "disk.mount"
#define HMON_METRIC_DISK_TOTAL_BYTES      "disk.total_bytes"
#define HMON_METRIC_DISK_FREE_BYTES       "disk.free_bytes"

/* NETWORK */
#define HMON_METRIC_NET_INTERFACE         "net.interface"
#define HMON_METRIC_NET_RX_KBPS           "net.rx_kbps"
#define HMON_METRIC_NET_TX_KBPS           "net.tx_kbps"

/* GPU */
#define HMON_METRIC_GPU_NAME              "gpu.%d.name"
#define HMON_METRIC_GPU_SOURCE            "gpu.%d.source"
#define HMON_METRIC_GPU_TEMP_C            "gpu.%d.temp_c"
#define HMON_METRIC_GPU_CLOCK_MHZ         "gpu.%d.clock_mhz"
#define HMON_METRIC_GPU_USAGE_PCT         "gpu.%d.usage_pct"
#define HMON_METRIC_GPU_POWER_W           "gpu.%d.power_w"
#define HMON_METRIC_GPU_VRAM_USED_MIB     "gpu.%d.vram_used_mib"
#define HMON_METRIC_GPU_VRAM_TOTAL_MIB    "gpu.%d.vram_total_mib"
#define HMON_METRIC_GPU_VRAM_USAGE_PCT    "gpu.%d.vram_usage_pct"
#define HMON_METRIC_GPU_IN_USE            "gpu.%d.in_use"

/* PROCESS */
#define HMON_METRIC_PROC_PID              "proc.%d.pid"
#define HMON_METRIC_PROC_CPU_PCT          "proc.%d.cpu_pct"
#define HMON_METRIC_PROC_MEM_PCT          "proc.%d.mem_pct"
#define HMON_METRIC_PROC_GPU_PCT          "proc.%d.gpu_pct"
#define HMON_METRIC_PROC_COMMAND          "proc.%d.command"

#ifdef __cplusplus
}
#endif

#endif /* HMON_PLUGIN_ABI_H */
