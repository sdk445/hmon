#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/mount.h>
#else
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "hmon/plugin_abi.h"
#include "hmon/plugin_manager.hpp"
#include "metrics/types.hpp"
#include "metrics/version.hpp"

#ifndef HMON_PLUGIN_DIR
#define HMON_PLUGIN_DIR "/usr/local/lib/hmon/plugins"
#endif

std::string currentTimestamp();
std::string hostName();
std::string humanBytes(unsigned long long bytes);
std::optional<double> getSwapUsagePercent();

std::string currentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  struct tm tm_buf;
  localtime_r(&t, &tm_buf);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
  return buf;
}

std::string hostName() {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) return buf;
  return "unknown";
}

std::string humanBytes(unsigned long long bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  int level = 0;
  double value = static_cast<double>(bytes);
  while (value >= 1024.0 && level < 4) {
    value /= 1024.0;
    ++level;
  }
  char buf[64];
  if (level == 0) {
    std::snprintf(buf, sizeof(buf), "%llu %s", bytes, units[level]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, units[level]);
  }
  return buf;
}

std::optional<double> getSwapUsagePercent() {
  std::ifstream f("/proc/meminfo");
  if (!f) return std::nullopt;
  std::string line;
  long long total = 0, free = 0;
  bool has_total = false;
  while (std::getline(f, line)) {
    if (line.rfind("SwapTotal:", 0) == 0) {
      std::istringstream iss(line.substr(10));
      if (iss >> total) has_total = true;
    } else if (line.rfind("SwapFree:", 0) == 0) {
      std::istringstream iss(line.substr(9));
      if (iss >> free) {}
    }
  }
  if (!has_total || total <= 0) return std::nullopt;
  long long used = total - free;
  return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}

enum class SortMode { kCpu, kMem, kGpu, kPid };
enum class ZenFocus { kNone, kPorts, kServices, kDocker };

struct Config {
  int refresh_interval_ms = 1000;
  size_t top_processes = 8;
  size_t history_points = 2048;
  bool show_gpu = true;
  bool show_history = true;
  bool zen_mode = false;
  int lock_pid = -1;
  int selected_pid = -1;
  SortMode sort_mode = SortMode::kCpu;
  bool show_colors = true;
  bool show_help = false;
  bool show_version = false;
  bool show_selection_highlight = false;
  int zen_docker_scroll = 0;
  int zen_ports_scroll = 0;
  int zen_services_scroll = 0;
  ZenFocus zen_focus = ZenFocus::kNone;
  std::optional<std::string> cli_error;
};

struct Rect {
  int y;
  int x;
  int h;
  int w;
};

struct MetricsHistory {
  std::vector<double> cpu_usage;
  std::vector<double> cpu_temp;
  std::vector<double> ram_usage;
  std::vector<double> gpu_usage;
  std::vector<double> gpu_vram_usage;
  std::vector<double> disk_usage;
};

struct BrailleCanvas {
  int width = 0;
  int height = 0;
  std::vector<uint16_t> cells;
};

constexpr uint16_t kDirUp = 0x01;
constexpr uint16_t kDirDown = 0x02;
constexpr uint16_t kDirLeft = 0x04;
constexpr uint16_t kDirRight = 0x08;
constexpr uint16_t kDirPoint = 0x10;

void printHelp(const char* program_name) {
  std::cout << "hmon " << version::kCurrent << "\n\n";
  std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help              Show this help and exit\n";
  std::cout << "  -v, --version           Show version and exit\n";
  std::cout << "  -r, --refresh <secs>    Refresh interval in seconds (1-60, default: 1)\n";
  std::cout << "  -t, --top <count>       Number of processes to show (1-20, default: 8)\n";
  std::cout << "  --no-gpu                Disable GPU\n";
  std::cout << "  --no-history            Disable history\n";
  std::cout << "  --zen                   Zen mode\n";
  std::cout << "  --pid <id>              Focus on a specific PID\n";
  std::cout << "  --no-color              Disable colors\n\n";
  std::cout << "Controls:\n";
  std::cout << "  q       Quit    ?       Help    z       Zen mode\n";
  std::cout << "  s       Sort    l       Lock    u       Unlock\n";
  std::cout << "  r       Refresh +/-     Speed\n";
}

void printVersion() {
  std::cout << "hmon " << version::kCurrent << "\n";
}

bool parseIntArg(const char* value, int min, int max, int* out) {
  if (!value || !out) {
    return false;
  }
  try {
    const int parsed = std::stoi(value);
    if (parsed < min || parsed > max) {
      return false;
    }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

Config parseArgs(int argc, char* argv[]) {
  Config config;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      config.show_help = true;
      return config;
    }

    if (arg == "-v" || arg == "--version") {
      config.show_version = true;
      return config;
    }

    if (arg == "-r" || arg == "--refresh") {
      if (i + 1 >= argc) {
        config.cli_error = "--refresh requires a value between 1 and 60.";
        return config;
      }
      int secs = 0;
      if (!parseIntArg(argv[++i], 1, 60, &secs)) {
        config.cli_error = "Invalid refresh interval. Use a value between 1 and 60 seconds.";
        return config;
      }
      config.refresh_interval_ms = secs * 1000;
      continue;
    }

    if (arg == "-t" || arg == "--top") {
      if (i + 1 >= argc) {
        config.cli_error = "--top requires a value between 1 and 20.";
        return config;
      }
      int count = 0;
      if (!parseIntArg(argv[++i], 1, 20, &count)) {
        config.cli_error = "Invalid top count. Use a value between 1 and 20.";
        return config;
      }
      config.top_processes = static_cast<size_t>(count);
      continue;
    }

    if (arg == "--no-gpu") {
      config.show_gpu = false;
      continue;
    }

    if (arg == "--no-history") {
      config.show_history = false;
      continue;
    }

    if (arg == "--zen") {
      config.zen_mode = true;
      continue;
    }

    if (arg == "--pid") {
      if (i + 1 >= argc) {
        config.cli_error = "--pid requires a positive process id.";
        return config;
      }
      int pid = 0;
      if (!parseIntArg(argv[++i], 1, std::numeric_limits<int>::max(), &pid)) {
        config.cli_error = "Invalid PID. Use a positive integer.";
        return config;
      }
      config.lock_pid = pid;
      continue;
    }

    if (arg == "--no-color") {
      config.show_colors = false;
      continue;
    }

    config.cli_error = "Unknown option: " + arg;
    return config;
  }

  return config;
}

std::string formatOptional(const std::optional<double>& value, const std::string& unit,
                           int precision = 1) {
  if (!value) {
    return "N/A";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << *value << unit;
  return out.str();
}

std::string formatCpuFrequency(const std::optional<double>& mhz) {
  if (!mhz) {
    return "N/A";
  }
  std::ostringstream out;
  if (*mhz >= 1000.0) {
    out << std::fixed << std::setprecision(2) << (*mhz / 1000.0) << " GHz";
  } else {
    out << std::fixed << std::setprecision(0) << *mhz << " MHz";
  }
  return out.str();
}

std::string formatCpuTopology(const std::optional<int>& cores, const std::optional<int>& threads) {
  if (!cores && !threads) {
    return "N/A";
  }
  const std::string cores_str = cores ? std::to_string(*cores) : "N/A";
  const std::string threads_str = threads ? std::to_string(*threads) : "N/A";
  return cores_str + "C / " + threads_str + "T";
}

std::string formatMibOrGib(const std::optional<double>& mib_value) {
  if (!mib_value) {
    return "N/A";
  }
  std::ostringstream out;
  if (*mib_value >= 1024.0) {
    out << std::fixed << std::setprecision(2) << (*mib_value / 1024.0) << " GiB";
  } else {
    out << std::fixed << std::setprecision(0) << *mib_value << " MiB";
  }
  return out.str();
}

std::string formatGpuVramUsage(const std::optional<double>& used, const std::optional<double>& total) {
  if (!used || !total) {
    return "N/A";
  }
  return formatMibOrGib(used) + " / " + formatMibOrGib(total);
}

bool processListContainsPid(const std::vector<ProcessInfo>& processes, int pid) {
  if (pid <= 0) {
    return false;
  }
  return std::any_of(processes.begin(), processes.end(),
                     [pid](const ProcessInfo& process) { return process.pid == pid; });
}

int selectedProcessIndex(const std::vector<ProcessInfo>& processes, int selected_pid) {
  if (processes.empty()) {
    return -1;
  }
  for (size_t i = 0; i < processes.size(); ++i) {
    if (processes[i].pid == selected_pid) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void syncSelection(const std::vector<ProcessInfo>& processes, Config* config) {
  if (!config) {
    return;
  }
  if (processes.empty()) {
    config->selected_pid = -1;
    return;
  }
  if (processListContainsPid(processes, config->selected_pid)) {
    return;
  }
  if (processListContainsPid(processes, config->lock_pid)) {
    config->selected_pid = config->lock_pid;
    return;
  }
  config->selected_pid = processes.front().pid;
}

void moveSelection(const std::vector<ProcessInfo>& processes, Config* config, int delta) {
  if (!config || processes.empty()) {
    return;
  }
  const int current_index = selectedProcessIndex(processes, config->selected_pid);
  const int next_index =
      std::max(0, std::min(static_cast<int>(processes.size()) - 1, current_index + delta));
  config->selected_pid = processes[static_cast<size_t>(next_index)].pid;
}

bool gpuHasTelemetry(const GpuMetrics& gpu) {
  return gpu.temperature_c || gpu.core_clock_mhz || gpu.utilization_percent || gpu.power_w ||
         gpu.memory_used_mib || gpu.memory_total_mib || gpu.memory_utilization_percent;
}

bool anyGpuHasTelemetry(const std::vector<GpuMetrics>& gpus) {
  return std::any_of(gpus.begin(), gpus.end(), [](const GpuMetrics& gpu) { return gpuHasTelemetry(gpu); });
}

size_t pickDisplayGpuIndex(const std::vector<GpuMetrics>& gpus) {
  if (gpus.empty()) {
    return 0;
  }
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (gpuHasTelemetry(gpus[i])) {
      return i;
    }
  }
  return 0;
}

std::optional<size_t> pickInUseGpuIndex(const std::vector<GpuMetrics>& gpus) {
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (gpus[i].in_use && *gpus[i].in_use) {
      return i;
    }
  }
  if (gpus.empty()) {
    return std::nullopt;
  }
  return pickDisplayGpuIndex(gpus);
}

int colorPairForPercent(double percent) {
  if (percent >= 85.0) {
    return 3;
  }
  if (percent >= 65.0) {
    return 2;
  }
  return 1;
}

void addWindowLine(WINDOW* win, int row, const std::string& text) {
  if (!win) {
    return;
  }
  const int max_y = getmaxy(win);
  const int max_x = getmaxx(win);
  if (row <= 0 || row >= max_y - 1 || max_x <= 4) {
    return;
  }
  mvwaddnstr(win, row, 2, text.c_str(), max_x - 4);
}

void addColoredText(WINDOW* win, int row, int col, const std::string& text, int color_pair) {
  if (!win) {
    return;
  }
  const int max_y = getmaxy(win);
  const int max_x = getmaxx(win);
  if (row <= 0 || row >= max_y - 1 || col < 1 || col >= max_x - 1) {
    return;
  }

  if (has_colors()) {
    wattron(win, COLOR_PAIR(color_pair));
  }
  mvwaddnstr(win, row, col, text.c_str(), max_x - 1 - col);
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(color_pair));
  }
}

void drawMiniBar(WINDOW* win, int row, int col, double percent, int width) {
  if (!win || width < 1) return;
  
  const double clamped = std::max(0.0, std::min(100.0, percent));
  const int filled = static_cast<int>(std::round((clamped / 100.0) * static_cast<double>(width)));
  const int color = colorPairForPercent(clamped);
  
  if (has_colors()) {
    wattron(win, COLOR_PAIR(color));
  }
  wattron(win, A_BOLD);
  
  for (int i = 0; i < width; i++) {
    if (i < filled) {
      mvwaddch(win, row, col + i, ACS_CKBOARD);
    } else {
      mvwaddch(win, row, col + i, ' ');
    }
  }
  
  wattroff(win, A_BOLD);
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(color));
  }
}

void drawBar(WINDOW* win, int row, const std::string& label, double percent, int type = 0) {
  if (!win) {
    return;
  }
  const int max_y = getmaxy(win);
  const int max_x = getmaxx(win);
  std::string unit = (type > 0) ? "C" : "%";
  
  if (row <= 0 || row >= max_y - 1 || max_x < 25) {
    addWindowLine(win, row, label + ": " +
    std::to_string(static_cast<int>(std::round(percent))) + unit);
    return;
  }

  const double clamped = std::max(0.0, std::min(100.0, percent));
  const std::string value_text = std::to_string(static_cast<int>(std::round(clamped))) + unit;
  const std::string prefix = label;
  const int suffix_width = static_cast<int>(value_text.size()) + 1;
  int inner_width = max_x - 4 - static_cast<int>(prefix.size()) - suffix_width - 3;
  if (inner_width < 8) {
    inner_width = 8;
  }
  const int filled = static_cast<int>(std::round((clamped / 100.0) * static_cast<double>(inner_width)));

  const int start_col = 2;
  mvwaddnstr(win, row, start_col, prefix.c_str(), max_x - 4);
  mvwaddch(win, row, start_col + static_cast<int>(prefix.size()), '[');
  const int bar_col = start_col + static_cast<int>(prefix.size()) + 1;

  wattron(win, A_BOLD);
  for (int i = 0; i < inner_width; ++i) {
    if (i < filled) {
      if (has_colors()) {
        wattron(win, COLOR_PAIR(colorPairForPercent(clamped)));
      }
      mvwaddch(win, row, bar_col + i, ACS_CKBOARD);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(colorPairForPercent(clamped)));
      }
    } else {
      mvwaddch(win, row, bar_col + i, ' ');
    }
  }
  wattroff(win, A_BOLD);

  mvwaddch(win, row, bar_col + inner_width, ']');
  wattron(win, A_BOLD);
  if (has_colors()) {
    wattron(win, COLOR_PAIR(4));
  }
  mvwaddnstr(win, row, bar_col + inner_width + 1, (" " + value_text).c_str(), max_x - 2 - (bar_col + inner_width + 1));
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(4));
  }
  wattroff(win, A_BOLD);
}

WINDOW* createPanel(const Rect& rect, const std::string& title) {
  if (rect.h < 4 || rect.w < 20) {
    return nullptr;
  }
  WINDOW* panel = newwin(rect.h, rect.w, rect.y, rect.x);
  
  if (has_colors()) {
    wattron(panel, COLOR_PAIR(4));
    wattron(panel, A_BOLD);
  }
  box(panel, ACS_VLINE, ACS_HLINE);
  if (has_colors()) {
    wattroff(panel, COLOR_PAIR(4));
    wattroff(panel, A_BOLD);
  }
  
  mvwprintw(panel, 0, 2, " %s ", title.c_str());
  return panel;
}

BrailleCanvas createBrailleCanvas(int width, int height) {
  BrailleCanvas canvas;
  canvas.width = std::max(0, width);
  canvas.height = std::max(0, height);
  canvas.cells.assign(static_cast<size_t>(canvas.width * canvas.height), 0);
  return canvas;
}

void connectCanvasCells(BrailleCanvas* canvas, int x0, int y0, int x1, int y1) {
  if (!canvas || !canvas->width || !canvas->height) {
    return;
  }
  if (x0 < 0 || x0 >= canvas->width || y0 < 0 || y0 >= canvas->height) {
    return;
  }
  if (x1 < 0 || x1 >= canvas->width || y1 < 0 || y1 >= canvas->height) {
    return;
  }
  
  const size_t idx0 = static_cast<size_t>(y0 * canvas->width + x0);
  const size_t idx1 = static_cast<size_t>(y1 * canvas->width + x1);
  
  if (x1 == x0 + 1 && y1 == y0) {
    canvas->cells[idx0] |= kDirRight;
    canvas->cells[idx1] |= kDirLeft;
  } else if (x1 == x0 - 1 && y1 == y0) {
    canvas->cells[idx0] |= kDirLeft;
    canvas->cells[idx1] |= kDirRight;
  } else if (x1 == x0 && y1 == y0 + 1) {
    canvas->cells[idx0] |= kDirDown;
    canvas->cells[idx1] |= kDirUp;
  } else if (x1 == x0 && y1 == y0 - 1) {
    canvas->cells[idx0] |= kDirUp;
    canvas->cells[idx1] |= kDirDown;
  }
}

void rasterizeBrailleLine(BrailleCanvas* canvas, int x0, int y0, int x1, int y1) {
  if (!canvas || !canvas->width || !canvas->height) return;
  if (x0 < 0 || x0 >= canvas->width || y0 < 0 || y0 >= canvas->height) return;
  if (x1 < 0 || x1 >= canvas->width || y1 < 0 || y1 >= canvas->height) return;
  
  int x = x0, y = y0;
  canvas->cells[static_cast<size_t>(y * canvas->width + x)] |= kDirPoint;
  
  const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int error = dx - dy;

  while (x != x1 || y != y1) {
    const int prev_x = x, prev_y = y;
    const int twice_error = error * 2;
    bool moved_x = false, moved_y = false;
    
    if (twice_error > -dy) { error -= dy; x += sx; moved_x = true; }
    if (twice_error < dx) { error += dx; y += sy; moved_y = true; }

    if (moved_x && moved_y) {
      connectCanvasCells(canvas, prev_x, prev_y, x, prev_y);
      connectCanvasCells(canvas, x, prev_y, x, y);
    } else {
      connectCanvasCells(canvas, prev_x, prev_y, x, y);
    }
    canvas->cells[static_cast<size_t>(y * canvas->width + x)] |= kDirPoint;
  }
}

void plotBrailleSeries(BrailleCanvas* canvas, const std::vector<double>& values,
                       double min_value, double max_value) {
  if (!canvas || canvas->width <= 0 || canvas->height <= 0 || values.empty()) return;
  if (max_value <= min_value) return;

  const int graph_w = canvas->width, graph_h = canvas->height;
  const size_t sample_count = std::min(values.size(), static_cast<size_t>(graph_w));
  const size_t start_index = values.size() - sample_count;
  if (sample_count == 0) return;

  auto valueToPixelY = [&](double value) {
    const double clamped = std::max(min_value, std::min(max_value, value));
    const double normalized = (clamped - min_value) / (max_value - min_value);
    const int y_from_bottom = static_cast<int>(std::lround(normalized * static_cast<double>(graph_h - 1)));
    return std::max(0, std::min(graph_h - 1, (graph_h - 1) - y_from_bottom));
  };

  auto sampleToPixelX = [&](size_t sample_index) {
    if (sample_count <= 1) return 0;
    return static_cast<int>((sample_index * static_cast<size_t>(graph_w - 1)) / (sample_count - 1));
  };

  if (sample_count == 1) {
    canvas->cells[static_cast<size_t>(valueToPixelY(values[start_index]) * canvas->width)] |= kDirPoint;
    return;
  }

  for (size_t i = 0; i + 1 < sample_count; ++i) {
    rasterizeBrailleLine(canvas, sampleToPixelX(i), valueToPixelY(values[start_index + i]),
                         sampleToPixelX(i + 1), valueToPixelY(values[start_index + i + 1]));
  }
}

void drawBrailleLayer(WINDOW* win, const BrailleCanvas& canvas, int top, int left, int color_pair) {
  if (!win || canvas.width <= 0 || canvas.height <= 0 || canvas.cells.empty()) return;

  if (has_colors()) {
    wattron(win, COLOR_PAIR(color_pair));
  }
  for (int cell_y = 0; cell_y < canvas.height; ++cell_y) {
    for (int cell_x = 0; cell_x < canvas.width; ++cell_x) {
      const uint16_t bits = canvas.cells[static_cast<size_t>(cell_y * canvas.width + cell_x)];
      if (bits == 0U) continue;
      
      wchar_t glyph = L' ';
      const uint16_t dirs = bits & static_cast<uint16_t>(kDirUp | kDirDown | kDirLeft | kDirRight);
      if (dirs == 0U) {
        glyph = (bits & kDirPoint) != 0U ? L'\u00b7' : L' ';
      } else if (dirs == (kDirLeft | kDirRight)) {
        glyph = L'\u2500';
      } else if (dirs == (kDirUp | kDirDown)) {
        glyph = L'\u2502';
      } else if (dirs == (kDirDown | kDirRight)) {
        glyph = L'\u250c';
      } else if (dirs == (kDirDown | kDirLeft)) {
        glyph = L'\u2510';
      } else if (dirs == (kDirUp | kDirRight)) {
        glyph = L'\u2514';
      } else if (dirs == (kDirUp | kDirLeft)) {
        glyph = L'\u2518';
      } else {
        glyph = L'\u253c';
      }
      const wchar_t glyph_str[2] = {glyph, L'\0'};
      mvwaddnwstr(win, top + cell_y, left + cell_x, glyph_str, 1);
    }
  }
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(color_pair));
  }
}

std::optional<double> computeRootDiskBusyPercent() {
  static std::optional<unsigned long long> previous_read_ops;
  static std::optional<unsigned long long> previous_write_ops;
  static std::chrono::steady_clock::time_point previous_time;

  std::string output;
  FILE* pipe = popen("iostat -d disk0 1 1 2>/dev/null | tail -1", "r");
  if (pipe) {
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);
  }
  if (output.empty()) {
    return std::nullopt;
  }

  std::istringstream iss(output);
  std::string device;
  unsigned long long read_ops = 0, write_ops = 0;
  
  iss >> device;
  if (device.empty()) return std::nullopt;
  
  std::vector<unsigned long long> values;
  unsigned long long val;
  while (iss >> val) {
    values.push_back(val);
  }
  
  if (values.size() < 2) return std::nullopt;
  
  read_ops = values[0];
  write_ops = values[1];
  
  const auto now = std::chrono::steady_clock::now();
  
  if (!previous_read_ops || !previous_write_ops) {
    previous_read_ops = read_ops;
    previous_write_ops = write_ops;
    previous_time = now;
    return std::nullopt;
  }
  
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous_time);
  const unsigned long long total_ops = (read_ops - *previous_read_ops) + (write_ops - *previous_write_ops);
  
  previous_read_ops = read_ops;
  previous_write_ops = write_ops;
  previous_time = now;
  
  if (elapsed.count() <= 0) return std::nullopt;
  
  const double ops_per_sec = static_cast<double>(total_ops) / (static_cast<double>(elapsed.count()) / 1000.0);
  const double busy_pct = std::min(100.0, ops_per_sec / 100.0 * 100.0);
  
  return std::max(0.0, std::min(100.0, busy_pct));
}

int estimateCpuRows(const Snapshot& snapshot) {
  int rows = 5;
  if (snapshot.cpu.usage_percent) ++rows;
  if (snapshot.cpu.temperature_c) ++rows;
  return rows;
}

int estimateRamRows() { return 4; }
int estimateNetworkRows() { return 4; }

int estimateGpuRows(const Snapshot& snapshot) {
  if (snapshot.gpus.empty()) return 3;
  if (!anyGpuHasTelemetry(snapshot.gpus)) {
    return static_cast<int>(std::min(snapshot.gpus.size(), static_cast<size_t>(5)));
  }
  return 7;
}

int estimateDiskRows() { return 4; }
int estimateHistoryRows() { return 12; }

void splitColumnHeights(int total_h, int top_pref_h, int bottom_pref_h, int gap, int* top_h, int* bottom_h) {
  const int min_panel_h = 4;
  const int available = std::max(min_panel_h * 2, total_h - gap);
  const int pref_sum = std::max(1, top_pref_h + bottom_pref_h);

  int proposed_top = static_cast<int>(std::round(static_cast<double>(available) * 
                                                  static_cast<double>(top_pref_h) / 
                                                  static_cast<double>(pref_sum)));
  proposed_top = std::max(min_panel_h, std::min(available - min_panel_h, proposed_top));

  *top_h = proposed_top;
  *bottom_h = available - proposed_top;
}

void renderCpuPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) return;
  
  int row = 1;
  const int max_y = getmaxy(panel);
  
  wattron(panel, A_BOLD);
  if (has_colors()) {
    wattron(panel, COLOR_PAIR(4));
  }
  mvwaddstr(panel, row++, 2, snapshot.cpu.name.c_str());
  if (has_colors()) {
    wattroff(panel, COLOR_PAIR(4));
  }
  wattroff(panel, A_BOLD);
  
  addWindowLine(panel, row++, "Topology: " + formatCpuTopology(snapshot.cpu.total_cores, snapshot.cpu.total_threads));
  addWindowLine(panel, row++, "Speed: " + formatCpuFrequency(snapshot.cpu.frequency_mhz));

  if (snapshot.cpu.usage_percent && row < max_y - 1) {
    drawBar(panel, row++, "Usage", *snapshot.cpu.usage_percent);
  }
  if (snapshot.cpu.temperature_c && row < max_y - 1) {
    drawBar(panel, row++, "Temp ", *snapshot.cpu.temperature_c, 1);
  }
}

void renderNetworkPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) return;
  
  int row = 1;

  const auto& net = snapshot.network;
  if (net.interface.empty()) {
    addWindowLine(panel, row++, "N/A");
    return;
  }

  wattron(panel, A_BOLD);
  addWindowLine(panel, row++, "Interface: " + net.interface);
  wattroff(panel, A_BOLD);
  
  if (has_colors()) {
    wattron(panel, COLOR_PAIR(1));
  }
  addWindowLine(panel, row++, "RX: " + formatOptional(net.rx_kbps, " KB/s", 1));
  if (has_colors()) {
    wattroff(panel, COLOR_PAIR(1));
  }
  
  if (has_colors()) {
    wattron(panel, COLOR_PAIR(3));
  }
  addWindowLine(panel, row++, "TX: " + formatOptional(net.tx_kbps, " KB/s", 1));
  if (has_colors()) {
    wattroff(panel, COLOR_PAIR(3));
  }
}

void renderRamPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) return;
  
  int row = 1;
  const int max_y = getmaxy(panel);

  if (!snapshot.ram.total_kb || !snapshot.ram.available_kb || *snapshot.ram.total_kb <= 0) {
    addWindowLine(panel, row++, "N/A");
    return;
  }

  const long long total_kb = *snapshot.ram.total_kb;
  const long long available_kb = std::max(0LL, *snapshot.ram.available_kb);
  const long long used_kb = std::max(0LL, total_kb - available_kb);

  const unsigned long long total_bytes = static_cast<unsigned long long>(total_kb) * 1024ULL;
  const unsigned long long used_bytes = static_cast<unsigned long long>(used_kb) * 1024ULL;
  const unsigned long long available_bytes = static_cast<unsigned long long>(available_kb) * 1024ULL;

  const double used_pct = (total_kb > 0) ? (100.0 * static_cast<double>(used_kb) / 
                                             static_cast<double>(total_kb)) : 0.0;

  addWindowLine(panel, row++, "Used: " + humanBytes(used_bytes) + " / " + humanBytes(total_bytes));
  addWindowLine(panel, row++, "Available: " + humanBytes(available_bytes));
  if (row < max_y - 1) {
    drawBar(panel, row++, "Usage", used_pct);
  }
}

void renderGpuPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) return;
  
  int row = 1;
  const int max_y = getmaxy(panel);

  if (!snapshot.gpus.empty()) {
    const size_t display_gpu_index = pickDisplayGpuIndex(snapshot.gpus);
    const GpuMetrics& gpu = snapshot.gpus[display_gpu_index];
    const auto in_use_gpu_index = pickInUseGpuIndex(snapshot.gpus);

    if (!anyGpuHasTelemetry(snapshot.gpus)) {
      size_t listed = 0;
      for (size_t i = 0; i < snapshot.gpus.size() && row < max_y - 1; ++i) {
        const GpuMetrics& item = snapshot.gpus[i];
        std::string line = item.name + " [" + item.source + "]";
        if (in_use_gpu_index && *in_use_gpu_index == i) {
          line += " (in use)";
        }
        addWindowLine(panel, row++, line);
        ++listed;
      }

      if (listed < snapshot.gpus.size() && row < max_y - 1) {
        const size_t remaining = snapshot.gpus.size() - listed;
        if (remaining > 1) {
          addWindowLine(panel, row++, "+" + std::to_string(remaining) + " more GPU(s)");
        }
      }
      return;
    }

    std::string gpu_source = gpu.source;
    if (gpu.source == "nvidia-smi") {
      gpu_source = "CUDA";
    }
    std::string gpu_header = "GPU: " + gpu.name + " [" + gpu_source + "]";
    if (in_use_gpu_index && *in_use_gpu_index == display_gpu_index) {
      gpu_header += " (in use)";
    }
    
    wattron(panel, A_BOLD);
    addWindowLine(panel, row++, gpu_header);
    wattroff(panel, A_BOLD);

    addWindowLine(panel, row++, "Temperature: " + formatOptional(gpu.temperature_c, " C", 1));
    addWindowLine(panel, row++, "Speed: " + formatOptional(gpu.core_clock_mhz, " MHz", 0));
    addWindowLine(panel, row++, "Power: " + formatOptional(gpu.power_w, " W", 1));
    addWindowLine(panel, row++, "VRAM: " + formatGpuVramUsage(gpu.memory_used_mib, gpu.memory_total_mib));

    if (!gpu.memory_used_mib && row < max_y - 1) {
      addWindowLine(panel, row++, "VRAM source not exposed");
    }

    if (gpu.utilization_percent && row < max_y - 1) {
      drawBar(panel, row++, "Util", *gpu.utilization_percent);
    }
    if (gpu.memory_utilization_percent && row < max_y - 1) {
      drawBar(panel, row++, "VRAM", *gpu.memory_utilization_percent);
    }
  } else {
    addWindowLine(panel, row++, "No GPU telemetry found");
  }
}

void renderDiskPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) return;
  
  int row = 1;
  const int max_y = getmaxy(panel);
  
  addWindowLine(panel, row++, "Mount: " + snapshot.disk.mount_point);

  if (!snapshot.disk.total_bytes || !snapshot.disk.free_bytes || *snapshot.disk.total_bytes == 0) {
    addWindowLine(panel, row++, "Disk data unavailable");
    return;
  }

  const auto total = *snapshot.disk.total_bytes;
  const auto free = std::min(*snapshot.disk.free_bytes, total);
  const auto used = total - free;

  const double used_pct = 100.0 * static_cast<double>(used) / static_cast<double>(total);

  addWindowLine(panel, row++, "Free: " + humanBytes(free) + " / " + humanBytes(total));
  if (row < max_y - 1) {
    drawBar(panel, row++, "Used", used_pct);
  }
}

void renderHistoryPanel(WINDOW* panel, const MetricsHistory& history,
                        const std::vector<ProcessInfo>& processes, int selected_pid, int lock_pid,
                        bool show_selection_highlight, SortMode sort_mode) {
  if (!panel) return;

  const int max_y = getmaxy(panel);
  const int max_x = getmaxx(panel);
  if (max_y < 6 || max_x < 30) {
    addWindowLine(panel, 1, "Expand terminal to view history.");
    return;
  }

  int row = 1;
  const int legend_row = row++;
  int legend_col = 2;
  
  const auto addLegendItem = [&](const std::string& text, int pair) {
    addColoredText(panel, legend_row, legend_col, text + " ", pair);
    const int line_start = legend_col + static_cast<int>(text.size()) + 1;
    if (has_colors()) {
      wattron(panel, COLOR_PAIR(pair));
    }
    for (int i = 0; i < 4 && line_start + i < max_x - 1; ++i) {
      mvwaddch(panel, legend_row, line_start + i, ACS_HLINE);
    }
    if (has_colors()) {
      wattroff(panel, COLOR_PAIR(pair));
    }
    legend_col += static_cast<int>(text.size()) + 7;
  };
  
  addLegendItem("CPU", 4);
  addLegendItem("TEMP", 3);
  addLegendItem("RAM", 2);
  addLegendItem("GPU", 1);
  addLegendItem("DISK", 6);

  const int graph_top = row;
  const int min_graph_h = 4;
  int max_table_rows = (max_y - 2) - (graph_top + min_graph_h);
  max_table_rows = std::max(0, max_table_rows);
  const int table_rows = std::min(8, max_table_rows);
  const bool has_table = table_rows >= 2;
  int table_top = max_y - 1 - table_rows;
  if (!has_table) {
    table_top = max_y - 1;
  }
  const int graph_bottom = table_top - 2;
  const int graph_h = graph_bottom - graph_top + 1;
  const int graph_left = 7;
  const int graph_right = max_x - 3;
  const int graph_w = graph_right - graph_left + 1;
  
  if (graph_w < 10 || graph_h < 3) {
    addWindowLine(panel, row, "Not enough space for trend graph.");
    return;
  }

  const int mid_y = graph_top + graph_h / 2;
  mvwprintw(panel, graph_top, 2, "100");
  mvwprintw(panel, mid_y, 3, "50");
  mvwprintw(panel, graph_bottom, 4, "0");
  
  for (int x = graph_left; x <= graph_right; ++x) {
    mvwaddch(panel, graph_top, x, ACS_HLINE);
    mvwaddch(panel, mid_y, x, ACS_HLINE);
    mvwaddch(panel, graph_bottom, x, ACS_HLINE);
  }

  BrailleCanvas cpu_cells = createBrailleCanvas(graph_w, graph_h);
  BrailleCanvas cpu_temp_cells = createBrailleCanvas(graph_w, graph_h);
  BrailleCanvas ram_cells = createBrailleCanvas(graph_w, graph_h);
  BrailleCanvas gpu_cells = createBrailleCanvas(graph_w, graph_h);
  BrailleCanvas disk_cells = createBrailleCanvas(graph_w, graph_h);

  plotBrailleSeries(&cpu_cells, history.cpu_usage, 0.0, 100.0);
  plotBrailleSeries(&cpu_temp_cells, history.cpu_temp, 0.0, 100.0);
  plotBrailleSeries(&ram_cells, history.ram_usage, 0.0, 100.0);
  plotBrailleSeries(&gpu_cells, history.gpu_usage, 0.0, 100.0);
  plotBrailleSeries(&disk_cells, history.disk_usage, 0.0, 100.0);

  drawBrailleLayer(panel, disk_cells, graph_top, graph_left, 6);
  drawBrailleLayer(panel, ram_cells, graph_top, graph_left, 2);
  drawBrailleLayer(panel, cpu_temp_cells, graph_top, graph_left, 3);
  drawBrailleLayer(panel, gpu_cells, graph_top, graph_left, 1);
  drawBrailleLayer(panel, cpu_cells, graph_top, graph_left, 4);

  if (!has_table) {
    return;
  }

  const int separator_row = table_top - 1;
  for (int x = 1; x < max_x - 1; ++x) {
    mvwaddch(panel, separator_row, x, ACS_HLINE);
  }
  for (int x = 1; x < max_x - 1; ++x) {
    mvwaddch(panel, table_top - 2, x, ACS_HLINE);
  }

  int table_row = table_top;
  bool show_gpu_col = true;

  wattron(panel, A_BOLD);
  int col = 2;
  std::string pid_text = "PID";
  std::string cpu_text = "CPU%";
  std::string mem_text = "MEM%";
  std::string gpu_text = "GPU%";
  
  if (sort_mode == SortMode::kPid) {
    if (has_colors()) wattron(panel, COLOR_PAIR(4));
    wattron(panel, A_UNDERLINE);
  }
  mvwaddstr(panel, table_row, col, pid_text.c_str());
  if (sort_mode == SortMode::kPid) {
    wattroff(panel, A_UNDERLINE);
    if (has_colors()) wattroff(panel, COLOR_PAIR(4));
  }
  col += 8;
  
  if (sort_mode == SortMode::kCpu) {
    if (has_colors()) wattron(panel, COLOR_PAIR(4));
    wattron(panel, A_UNDERLINE);
  }
  mvwaddstr(panel, table_row, col, cpu_text.c_str());
  if (sort_mode == SortMode::kCpu) {
    wattroff(panel, A_UNDERLINE);
    if (has_colors()) wattroff(panel, COLOR_PAIR(4));
  }
  col += 8;
  
  if (sort_mode == SortMode::kMem) {
    if (has_colors()) wattron(panel, COLOR_PAIR(4));
    wattron(panel, A_UNDERLINE);
  }
  mvwaddstr(panel, table_row, col, mem_text.c_str());
  if (sort_mode == SortMode::kMem) {
    wattroff(panel, A_UNDERLINE);
    if (has_colors()) wattroff(panel, COLOR_PAIR(4));
  }
  col += 8;
  
  if (show_gpu_col) {
    if (sort_mode == SortMode::kGpu) {
      if (has_colors()) wattron(panel, COLOR_PAIR(4));
      wattron(panel, A_UNDERLINE);
    }
    mvwaddstr(panel, table_row, col, gpu_text.c_str());
    if (sort_mode == SortMode::kGpu) {
      wattroff(panel, A_UNDERLINE);
      if (has_colors()) wattroff(panel, COLOR_PAIR(4));
    }
    col += 8;
  }
  
  mvwaddstr(panel, table_row, col, "COMMAND");
  table_row++;
  wattroff(panel, A_BOLD);

  const int max_entries = table_rows - 1;
  for (int i = 0; i < max_entries; ++i) {
    if (i >= static_cast<int>(processes.size())) {
      break;
    }
    const auto& process = processes[static_cast<size_t>(i)];
    const bool is_selected = process.pid == selected_pid;
    const bool is_locked = process.pid == lock_pid;
    const bool show_row_highlight = is_locked || (show_selection_highlight && is_selected && !is_locked);
    
    if (show_row_highlight) {
      if (has_colors()) {
        wattron(panel, COLOR_PAIR(5));
      }
      wattron(panel, A_REVERSE);
    }
    
    std::ostringstream line;
    line << std::setw(6) << process.pid << " "
         << std::setw(6) << std::fixed << std::setprecision(1) << process.cpu_percent << " "
         << std::setw(6) << std::fixed << std::setprecision(1) << process.mem_percent;
    if (show_gpu_col) {
      line << " " << std::setw(6) << std::fixed << std::setprecision(1) << process.gpu_percent;
    }
    line << " ";
    if (is_locked) {
      line << "*";
    } else {
      line << " ";
    }
    line << process.command;
    addWindowLine(panel, table_row++, line.str());
    
    if (show_row_highlight) {
      wattroff(panel, A_REVERSE);
      if (has_colors()) {
        wattroff(panel, COLOR_PAIR(5));
      }
    }
  }
}

void addClippedText(WINDOW* win, int row, int col, int width, const std::string& text) {
  if (!win || width <= 0) {
    return;
  }
  mvwaddnstr(win, row, col, text.c_str(), width);
}

void drawZenSectionHeader(WINDOW* win, int row, int col, int width,
                          const std::string& title, int color_pair) {
  if (!win || width <= 0) {
    return;
  }
  const int title_width = static_cast<int>(title.size());
  const int line_col = col + title_width + 1;
  const int line_width = std::max(0, width - title_width - 2);

  wattron(win, A_BOLD);
  if (has_colors()) {
    wattron(win, COLOR_PAIR(color_pair));
  }
  addClippedText(win, row, col, width, title);
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(color_pair));
  }
  wattroff(win, A_BOLD);

  if (line_width > 0) {
    if (has_colors()) {
      wattron(win, COLOR_PAIR(7));
    }
    mvwhline(win, row, line_col, ACS_HLINE, line_width);
    if (has_colors()) {
      wattroff(win, COLOR_PAIR(7));
    }
  }
}

std::string clippedText(const std::string& text, int width) {
  if (width <= 0) {
    return "";
  }
  if (static_cast<int>(text.size()) <= width) {
    return text;
  }
  if (width <= 3) {
    return text.substr(0, width);
  }
  return text.substr(0, width - 3) + "...";
}

std::string formatPercentText(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value << "%";
  return out.str();
}

void renderZenMode(WINDOW* win, const Snapshot& snapshot, const Config& config,
                   const std::vector<ProcessInfo>& processes) {
  if (!win) return;

  int max_y, max_x;
  getmaxyx(win, max_y, max_x);

  erase();

  const int margin = std::max(2, max_x / 40);
  const int content_w = std::max(20, max_x - margin * 2);
  int row = 1;

  drawZenSectionHeader(win, row++, margin, content_w, "CPU Cores", 4);
  row++;

  if (!snapshot.cpu.core_usage_percent.empty()) {
    const int item_width = 16;
    const int item_gap = 2;
    const int cores_per_row = std::max(1, (content_w + item_gap) / (item_width + item_gap));
    const int grid_width = cores_per_row * item_width + (cores_per_row - 1) * item_gap;
    const int grid_start = margin + std::max(0, (content_w - grid_width) / 2);

    for (size_t i = 0; i < snapshot.cpu.core_usage_percent.size(); ++i) {
      const int item_row = row + static_cast<int>(i / static_cast<size_t>(cores_per_row));
      if (item_row >= max_y - 6) {
        break;
      }

      const int col = grid_start + static_cast<int>(i % static_cast<size_t>(cores_per_row)) * (item_width + item_gap);
      const double usage = snapshot.cpu.core_usage_percent[i];
      const int color = colorPairForPercent(usage);
      std::ostringstream label;
      label << "C" << i;

      if (has_colors()) {
        wattron(win, COLOR_PAIR(color));
      }
      wattron(win, A_BOLD);
      addClippedText(win, item_row, col, item_width, label.str());
      wattroff(win, A_BOLD);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(color));
      }

      drawMiniBar(win, item_row, col + 3, usage, 7);

      std::ostringstream pct;
      pct << std::setw(4) << static_cast<int>(usage) << "%";
      if (has_colors()) {
        wattron(win, COLOR_PAIR(color));
      }
      addClippedText(win, item_row, col + 11, 5, pct.str());
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(color));
      }
    }

    row += static_cast<int>((snapshot.cpu.core_usage_percent.size() + static_cast<size_t>(cores_per_row) - 1) /
                            static_cast<size_t>(cores_per_row));
  } else {
    addClippedText(win, row++, margin + 2, content_w - 4, "Core usage data unavailable");
  }

  row += 2;

  const int column_gap = std::max(3, content_w / 30);
  const int left_w = std::min(std::max(42, content_w / 3), content_w / 2);
  const int right_w = content_w - left_w - column_gap;
  const int left_x = margin;
  const int right_x = left_x + left_w + column_gap;
  int left_row = row;
  int right_row = row;

  auto drawSummaryBar = [&](int target_row, int col, int width, const std::string& label,
                            double percent, const std::string& detail) {
    const int label_width = 7;
    const int pct_width = 7;
    const int bar_width = std::max(8, std::min(16, width / 3));
    const int detail_col = col + label_width + bar_width + pct_width + 4;

    addClippedText(win, target_row, col, label_width, label);
    drawMiniBar(win, target_row, col + label_width, percent, bar_width);

    if (has_colors()) {
      wattron(win, COLOR_PAIR(colorPairForPercent(percent)));
    }
    addClippedText(win, target_row, col + label_width + bar_width + 1, pct_width, formatPercentText(percent));
    if (has_colors()) {
      wattroff(win, COLOR_PAIR(colorPairForPercent(percent)));
    }
    addClippedText(win, target_row, detail_col, std::max(0, width - (detail_col - col)), detail);
  };

  if (config.show_gpu && !snapshot.gpus.empty()) {
    drawZenSectionHeader(win, left_row++, left_x, left_w, "GPU", 4);
    left_row++;

    for (size_t g = 0; g < snapshot.gpus.size() && left_row < max_y - 8; ++g) {
      const auto& gpu = snapshot.gpus[g];
      std::string title = clippedText(gpu.name, left_w - 2);
      wattron(win, A_BOLD);
      addClippedText(win, left_row, left_x, left_w, title);
      wattroff(win, A_BOLD);

      std::string source = "[" + gpu.source + "]";
      if (gpu.in_use && *gpu.in_use) {
        source += " (active)";
      }
      if (has_colors()) {
        wattron(win, COLOR_PAIR(7));
      }
      addClippedText(win, left_row, left_x + static_cast<int>(title.size()) + 1,
                     std::max(0, left_w - static_cast<int>(title.size()) - 1), source);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(7));
      }
      left_row++;

      if (gpu.utilization_percent) {
        drawSummaryBar(left_row++, left_x + 1, left_w - 2, "Util", *gpu.utilization_percent, "");
      }
      if (gpu.memory_utilization_percent) {
        drawSummaryBar(left_row++, left_x + 1, left_w - 2, "VRAM", *gpu.memory_utilization_percent,
                       formatGpuVramUsage(gpu.memory_used_mib, gpu.memory_total_mib));
      }

      std::vector<std::string> details;
      if (gpu.temperature_c) {
        details.push_back("Temp " + formatOptional(gpu.temperature_c, "C", 0));
      }
      if (gpu.power_w) {
        details.push_back("Power " + formatOptional(gpu.power_w, "W", 0));
      }
      if (gpu.core_clock_mhz) {
        if (*gpu.core_clock_mhz >= 1000.0) {
          std::ostringstream out;
          out << "Clock " << std::fixed << std::setprecision(2) << (*gpu.core_clock_mhz / 1000.0) << "GHz";
          details.push_back(out.str());
        } else {
          std::ostringstream out;
          out << "Clock " << std::fixed << std::setprecision(0) << *gpu.core_clock_mhz << "MHz";
          details.push_back(out.str());
        }
      }
      if (!details.empty()) {
        std::string line;
        for (size_t i = 0; i < details.size(); ++i) {
          if (!line.empty()) {
            line += "  |  ";
          }
          line += details[i];
        }
        addClippedText(win, left_row++, left_x + 1, left_w - 2, line);
      }
      left_row++;
    }
  }

  drawZenSectionHeader(win, left_row++, left_x, left_w, "Memory & Storage", 2);
  left_row++;

  if (snapshot.ram.total_kb && snapshot.ram.available_kb && *snapshot.ram.total_kb > 0) {
    const long long total_kb = *snapshot.ram.total_kb;
    const long long available_kb = std::max(0LL, *snapshot.ram.available_kb);
    const long long used_kb = total_kb - available_kb;
    const double ram_pct = 100.0 * static_cast<double>(used_kb) / static_cast<double>(total_kb);
    drawSummaryBar(left_row++, left_x, left_w, "RAM", ram_pct,
                   humanBytes(static_cast<unsigned long long>(used_kb) * 1024ULL));
  }

  if (const auto swap_pct = getSwapUsagePercent(); swap_pct && *swap_pct >= 0) {
    std::string swap_detail;
    if (snapshot.swap.total_kb && snapshot.swap.free_kb) {
      long long used_kb = *snapshot.swap.total_kb - *snapshot.swap.free_kb;
      swap_detail = humanBytes(static_cast<unsigned long long>(used_kb) * 1024ULL) + " / " +
                    humanBytes(static_cast<unsigned long long>(*snapshot.swap.total_kb) * 1024ULL);
    } else {
      swap_detail = formatPercentText(*swap_pct);
    }
    drawSummaryBar(left_row++, left_x, left_w, "Swap", *swap_pct, swap_detail);
  }

  if (snapshot.disk.total_bytes && snapshot.disk.free_bytes && *snapshot.disk.total_bytes > 0) {
    const auto total = *snapshot.disk.total_bytes;
    const auto free = std::min(*snapshot.disk.free_bytes, total);
    const auto used = total - free;
    const double used_pct = 100.0 * static_cast<double>(used) / static_cast<double>(total);
    drawSummaryBar(left_row++, left_x, left_w, "Disk", used_pct,
                   humanBytes(used));
  }

  if (!snapshot.network.interface.empty()) {
    drawZenSectionHeader(win, left_row++, left_x, left_w, "Network", 6);
    left_row++;
    char net_buf[128];
    std::snprintf(net_buf, sizeof(net_buf), "Interface: %s", snapshot.network.interface.c_str());
    addClippedText(win, left_row++, left_x + 1, left_w - 2, net_buf);
    if (snapshot.network.rx_kbps) {
      std::string rx_line = "RX: " + formatOptional(snapshot.network.rx_kbps, " KB/s", 1);
      addClippedText(win, left_row++, left_x + 1, left_w - 2, rx_line);
    }
    if (snapshot.network.tx_kbps) {
      std::string tx_line = "TX: " + formatOptional(snapshot.network.tx_kbps, " KB/s", 1);
      addClippedText(win, left_row++, left_x + 1, left_w - 2, tx_line);
    }
    left_row++;
  }

  if (!snapshot.docker_containers.empty()) {
    bool focused = (config.zen_focus == ZenFocus::kDocker);
    if (focused) {
      if (has_colors()) wattron(win, COLOR_PAIR(4));
      wattron(win, A_BOLD);
    }
    drawZenSectionHeader(win, left_row++, left_x, left_w, focused ? "> Docker" : "Docker", 1);
    if (focused) {
      wattroff(win, A_BOLD);
      if (has_colors()) wattroff(win, COLOR_PAIR(4));
    }
    left_row++;

    int max_show = 4;
    int total = static_cast<int>(snapshot.docker_containers.size());
    int start = focused ? std::max(0, std::min(config.zen_docker_scroll, total - max_show)) : 0;
    int shown = 0;
    for (int i = start; i < total && shown < max_show; ++i) {
      if (left_row >= max_y - 4) break;
      const auto& ct = snapshot.docker_containers[static_cast<size_t>(i)];

      wattron(win, A_BOLD);
      std::string ct_header = clippedText(ct.name, left_w - 2);
      addClippedText(win, left_row, left_x + 1, left_w - 2, ct_header);
      wattroff(win, A_BOLD);

      std::string img = "[" + clippedText(ct.image, left_w - 10) + "]";
      if (has_colors()) {
        wattron(win, COLOR_PAIR(7));
      }
      addClippedText(win, left_row, left_x + 1 + static_cast<int>(ct_header.size()) + 1,
                     std::max(0, left_w - 2 - static_cast<int>(ct_header.size()) - 1), img);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(7));
      }
      left_row++;

      if (left_row < max_y - 4) {
        drawSummaryBar(left_row, left_x + 2, left_w - 4, "CPU", ct.cpu_percent, "");
        left_row++;
      }
      if (left_row < max_y - 4) {
        drawSummaryBar(left_row, left_x + 2, left_w - 4, "MEM", ct.mem_percent,
                       humanBytes(ct.mem_usage));
        left_row++;
      }
      if (left_row < max_y - 4 && (ct.net_rx_total > 0 || ct.net_tx_total > 0)) {
        char net_total_buf[128];
        std::snprintf(net_total_buf, sizeof(net_total_buf), "Total: ↓%s ↑%s",
                      humanBytes(ct.net_rx_total).c_str(), humanBytes(ct.net_tx_total).c_str());
        addClippedText(win, left_row++, left_x + 2, left_w - 4, net_total_buf);
      }
      if (left_row < max_y - 4 && ct.pids_current > 0) {
        std::string pid_line = "PIDs: " + std::to_string(ct.pids_current);
        addClippedText(win, left_row++, left_x + 2, left_w - 4, pid_line);
      }
      left_row++;
      ++shown;
    }
    if (total > max_show) {
      char info[64];
      std::snprintf(info, sizeof(info), "  [%d-%d/%d] j/k scroll", start + 1, start + shown, total);
      if (focused && has_colors()) wattron(win, COLOR_PAIR(4));
      addClippedText(win, left_row++, left_x + 1, left_w - 2, info);
      if (focused && has_colors()) wattroff(win, COLOR_PAIR(4));
    }
  } else if (snapshot.docker_loading) {
    drawZenSectionHeader(win, left_row++, left_x, left_w, "Docker", 1);
    left_row++;
    addClippedText(win, left_row++, left_x + 1, left_w - 2, "Loading...");
    left_row++;
  }

  /* Databases — left column */
  if (!snapshot.databases.empty()) {
    drawZenSectionHeader(win, left_row++, left_x, left_w, "Databases", 2);
    left_row++;
    for (const auto& d : snapshot.databases) {
      if (left_row >= max_y - 4) break;
      if (d.status == "running") {
        char line[128];
        std::snprintf(line, sizeof(line), "  %-12s %d/%d conns",
                      d.type.c_str(), d.active_connections,
                      d.max_connections > 0 ? d.max_connections : d.active_connections);
        addClippedText(win, left_row++, left_x + 1, left_w - 2, line);
      } else {
        char line[64];
        std::snprintf(line, sizeof(line), "  %-12s stopped", d.type.c_str());
        if (has_colors()) wattron(win, COLOR_PAIR(3));
        addClippedText(win, left_row++, left_x + 1, left_w - 2, line);
        if (has_colors()) wattroff(win, COLOR_PAIR(3));
      }
    }
    left_row++;
  }

  /* Cron jobs — left column */
  if (!snapshot.cron_jobs.empty()) {
    drawZenSectionHeader(win, left_row++, left_x, left_w, "Cron", 7);
    left_row++;
    int shown = 0;
    for (const auto& c : snapshot.cron_jobs) {
      if (left_row >= max_y - 4 || shown >= 5) break;
      char line[256];
      std::string cmd = c.command;
      if (cmd.size() > 45) cmd = cmd.substr(0, 42) + "...";
      std::snprintf(line, sizeof(line), "  %-14s %s", c.schedule.c_str(), cmd.c_str());
      addClippedText(win, left_row++, left_x + 1, left_w - 2, line);
      ++shown;
    }
    left_row++;
  }

  /* Right column — Ports, Services, Web, Processes */

  /* Ports — scrollable */
  if (!snapshot.ports.empty()) {
    bool focused = (config.zen_focus == ZenFocus::kPorts);
    if (focused) {
      if (has_colors()) wattron(win, COLOR_PAIR(4));
      wattron(win, A_BOLD);
    }
    drawZenSectionHeader(win, right_row++, right_x, right_w, focused ? "> Ports" : "Ports", 6);
    if (focused) {
      wattroff(win, A_BOLD);
      if (has_colors()) wattroff(win, COLOR_PAIR(4));
    }
    right_row++;
    int max_show = 6;
    int total = static_cast<int>(snapshot.ports.size());
    int start = focused ? std::max(0, std::min(config.zen_ports_scroll, total - max_show)) : 0;
    int shown = 0;
    for (int i = start; i < total && shown < max_show; ++i) {
      if (right_row >= max_y - 4) break;
      const auto& p = snapshot.ports[static_cast<size_t>(i)];
      char line[128];
      std::snprintf(line, sizeof(line), "  %-5d %-4s %s",
                    p.port, p.proto.c_str(),
                    p.process.empty() ? "?" : p.process.c_str());
      if (focused && has_colors()) wattron(win, COLOR_PAIR(4));
      addClippedText(win, right_row++, right_x, right_w - 1, line);
      if (focused && has_colors()) wattroff(win, COLOR_PAIR(4));
      ++shown;
    }
    if (total > max_show) {
      char info[64];
      std::snprintf(info, sizeof(info), "  [%d-%d/%d] j/k scroll", start + 1, start + shown, total);
      if (focused && has_colors()) wattron(win, COLOR_PAIR(4));
      addClippedText(win, right_row++, right_x, right_w - 1, info);
      if (focused && has_colors()) wattroff(win, COLOR_PAIR(4));
    }
    right_row++;
  }

  /* Services — scrollable */
  if (!snapshot.services.empty()) {
    bool focused = (config.zen_focus == ZenFocus::kServices);
    if (focused) {
      if (has_colors()) wattron(win, COLOR_PAIR(4));
      wattron(win, A_BOLD);
    }
    drawZenSectionHeader(win, right_row++, right_x, right_w, focused ? "> Services" : "Services", 4);
    if (focused) {
      wattroff(win, A_BOLD);
      if (has_colors()) wattroff(win, COLOR_PAIR(4));
    }
    right_row++;
    int max_show = 6;
    int total = static_cast<int>(snapshot.services.size());
    int start = focused ? std::max(0, std::min(config.zen_services_scroll, total - max_show)) : 0;
    int shown = 0;
    for (int i = start; i < total && shown < max_show; ++i) {
      if (right_row >= max_y - 4) break;
      const auto& s = snapshot.services[static_cast<size_t>(i)];
      char line[128];
      const char* dot = (s.state == "failed") ? "!!" : "  ";
      int color = (s.state == "failed") ? 3 : (focused ? 4 : 1);
      std::snprintf(line, sizeof(line), "%s %-16s %s", dot, s.name.c_str(), s.sub_state.c_str());
      if (has_colors()) wattron(win, COLOR_PAIR(color));
      addClippedText(win, right_row++, right_x, right_w - 1, line);
      if (has_colors()) wattroff(win, COLOR_PAIR(color));
      ++shown;
    }
    if (total > max_show) {
      char info[64];
      std::snprintf(info, sizeof(info), "  [%d-%d/%d] j/k scroll", start + 1, start + shown, total);
      if (focused && has_colors()) wattron(win, COLOR_PAIR(4));
      addClippedText(win, right_row++, right_x, right_w - 1, info);
      if (focused && has_colors()) wattroff(win, COLOR_PAIR(4));
    }
    right_row++;
  }

  /* Web servers */
  if (!snapshot.webservers.empty()) {
    drawZenSectionHeader(win, right_row++, right_x, right_w, "Web", 4);
    right_row++;
    for (const auto& w : snapshot.webservers) {
      if (right_row >= max_y - 4) break;
      if (w.status == "running") {
        char line[128];
        std::snprintf(line, sizeof(line), "  %-8s %d active  %.0f r/s",
                      w.type.c_str(), w.active_connections, w.requests_per_sec);
        addClippedText(win, right_row++, right_x, right_w - 1, line);
      } else {
        char line[64];
        std::snprintf(line, sizeof(line), "  %-8s stopped", w.type.c_str());
        if (has_colors()) wattron(win, COLOR_PAIR(3));
        addClippedText(win, right_row++, right_x, right_w - 1, line);
        if (has_colors()) wattroff(win, COLOR_PAIR(3));
      }
    }
    right_row++;
  }

  drawZenSectionHeader(win, right_row++, right_x, right_w, "Top RAM Processes", 2);
  right_row += 2;

  std::vector<ProcessInfo> sorted_by_ram = processes;
  std::sort(sorted_by_ram.begin(), sorted_by_ram.end(),
            [](const ProcessInfo& a, const ProcessInfo& b) { return a.mem_percent > b.mem_percent; });

  const int pid_col = right_x;
  const int ram_col = pid_col + 10;
  const int pct_col = ram_col + 12;
  const int bar_col = pct_col + 7;
  const int bar_width = std::max(8, std::min(14, right_w / 8));
  const int cmd_col = bar_col + bar_width + 3;
  const int cmd_width = std::max(10, right_x + right_w - cmd_col);
  const int max_process_rows = std::max(3, max_y - right_row - 4);

  wattron(win, A_BOLD);
  addClippedText(win, right_row, pid_col, 8, "PID");
  addClippedText(win, right_row, ram_col, 10, "RAM");
  addClippedText(win, right_row, pct_col, 5, "%");
  addClippedText(win, right_row, cmd_col, cmd_width, "COMMAND");
  wattroff(win, A_BOLD);
  right_row++;

  long long total_ram_kb = snapshot.ram.total_kb.value_or(0);
  for (int i = 0; i < static_cast<int>(sorted_by_ram.size()) && i < max_process_rows; ++i) {
    const auto& proc = sorted_by_ram[static_cast<size_t>(i)];
    const bool show_row_highlight =
        proc.pid == config.lock_pid ||
        (config.show_selection_highlight && config.lock_pid <= 0 && proc.pid == config.selected_pid);
    const int color = colorPairForPercent(proc.mem_percent * 5.0);

    if (show_row_highlight) {
      if (has_colors()) {
        wattron(win, COLOR_PAIR(5));
      }
      wattron(win, A_REVERSE);
    }

    std::ostringstream pid_str;
    pid_str << std::setw(8) << proc.pid;
    addClippedText(win, right_row, pid_col, 8, pid_str.str());

    const long long proc_mem_kb = static_cast<long long>(proc.mem_percent * total_ram_kb / 100.0);
    const double proc_mem_mb = static_cast<double>(proc_mem_kb) / 1024.0;
    std::ostringstream mem_str;
    if (proc_mem_mb >= 1024.0) {
      mem_str << std::fixed << std::setprecision(1) << (proc_mem_mb / 1024.0) << "G";
    } else {
      mem_str << std::fixed << std::setprecision(0) << proc_mem_mb << "M";
    }
    if (has_colors()) {
      wattron(win, COLOR_PAIR(color));
    }
    wattron(win, A_BOLD);
    addClippedText(win, right_row, ram_col, 10, mem_str.str());
    wattroff(win, A_BOLD);

    addClippedText(win, right_row, pct_col, 6, formatPercentText(proc.mem_percent));
    if (has_colors()) {
      wattroff(win, COLOR_PAIR(color));
    }

    drawMiniBar(win, right_row, bar_col, proc.mem_percent * 5.0, bar_width);
    addClippedText(win, right_row, cmd_col, cmd_width, clippedText(proc.command, cmd_width));

    if (show_row_highlight) {
      wattroff(win, A_REVERSE);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(5));
      }
    }

    right_row++;
  }

  if (config.lock_pid > 0 && right_row < max_y - 2) {
    right_row++;
    if (has_colors()) {
      wattron(win, COLOR_PAIR(4));
    }
    addClippedText(win, right_row, right_x, right_w, "Locked on PID " + std::to_string(config.lock_pid));
    if (has_colors()) {
      wattroff(win, COLOR_PAIR(4));
    }
  }

  wnoutrefresh(win);
}

void drawHelpOverlay(WINDOW* overlay, const Config& config) {
  if (!overlay) return;

  int max_y = 0;
  int max_x = 0;
  getmaxyx(overlay, max_y, max_x);
  (void)max_y;

  box(overlay, ACS_VLINE, ACS_HLINE);

  const std::string title = " Controls ";
  mvwaddstr(overlay, 0, (max_x - title.size()) / 2, title.c_str());

  int row = 2;
  mvwaddstr(overlay, row++, 4, "q       Quit");
  mvwaddstr(overlay, row++, 4, "?       Toggle help");
  mvwaddstr(overlay, row++, 4, "z       Zen mode");
  mvwaddstr(overlay, row++, 4, "s       Sort (CPU/MEM/GPU/PID)");
  mvwaddstr(overlay, row++, 4, "j/k     Move selection");
  mvwaddstr(overlay, row++, 4, "Up/Down Move selection");
  mvwaddstr(overlay, row++, 4, "1-9     Jump to row");
  if (config.lock_pid > 0) {
    std::string pid_msg = "l/u     Toggle lock on PID " + std::to_string(config.lock_pid);
    mvwaddstr(overlay, row++, 4, pid_msg.c_str());
  } else if (config.selected_pid > 0) {
    std::string pid_msg = "l       Lock PID " + std::to_string(config.selected_pid);
    mvwaddstr(overlay, row++, 4, pid_msg.c_str());
  } else {
    mvwaddstr(overlay, row++, 4, "l       Lock selected PID");
  }
  mvwaddstr(overlay, row++, 4, "r       Refresh");
  mvwaddstr(overlay, row++, 4, "+/-     Speed");
  mvwaddstr(overlay, row++, 4, "Any key - Close help");

  wnoutrefresh(overlay);
}

Snapshot collectSnapshot(hmon::core::PluginManager& pm, const Config& config) {
  Snapshot snapshot;

  /* CPU metrics from plugin */
  snapshot.cpu.name = pm.get_string(HMON_METRIC_CPU_NAME, "Unknown CPU");
  auto cores = pm.get_int64(HMON_METRIC_CPU_CORES);
  if (cores) snapshot.cpu.total_cores = static_cast<int>(*cores);
  auto threads = pm.get_int64(HMON_METRIC_CPU_THREADS);
  if (threads) snapshot.cpu.total_threads = static_cast<int>(*threads);
  auto temp = pm.get_double(HMON_METRIC_CPU_TEMP_C);
  if (temp) snapshot.cpu.temperature_c = *temp;
  auto freq = pm.get_double(HMON_METRIC_CPU_FREQ_MHZ);
  if (freq) snapshot.cpu.frequency_mhz = *freq;
  auto usage = pm.get_double(HMON_METRIC_CPU_USAGE_PCT);
  if (usage) snapshot.cpu.usage_percent = *usage;

  auto core_metrics = pm.get_by_prefix("cpu.core_usage_pct.");
  for (const auto& m : core_metrics) {
    if (m.value.type == HMON_VAL_DOUBLE) {
      snapshot.cpu.core_usage_percent.push_back(m.value.v.f64);
    }
  }

  /* RAM metrics from plugin */
  auto ram_total = pm.get_int64(HMON_METRIC_RAM_TOTAL_KB);
  if (ram_total) snapshot.ram.total_kb = *ram_total;
  auto ram_avail = pm.get_int64(HMON_METRIC_RAM_AVAILABLE_KB);
  if (ram_avail) snapshot.ram.available_kb = *ram_avail;

  /* Swap metrics from plugin */
  auto swap_total = pm.get_int64("swap.total_kb");
  if (swap_total) snapshot.swap.total_kb = *swap_total;
  auto swap_free = pm.get_int64("swap.free_kb");
  if (swap_free) snapshot.swap.free_kb = *swap_free;

  /* Disk metrics from plugin */
  snapshot.disk.mount_point = pm.get_string(HMON_METRIC_DISK_MOUNT, "/");
  auto disk_total = pm.get_int64(HMON_METRIC_DISK_TOTAL_BYTES);
  if (disk_total) snapshot.disk.total_bytes = static_cast<unsigned long long>(*disk_total);
  auto disk_free = pm.get_int64(HMON_METRIC_DISK_FREE_BYTES);
  if (disk_free) snapshot.disk.free_bytes = static_cast<unsigned long long>(*disk_free);

  /* Network metrics from plugin */
  snapshot.network.interface = pm.get_string(HMON_METRIC_NET_INTERFACE);
  auto rx = pm.get_double(HMON_METRIC_NET_RX_KBPS);
  if (rx) snapshot.network.rx_kbps = *rx;
  auto tx = pm.get_double(HMON_METRIC_NET_TX_KBPS);
  if (tx) snapshot.network.tx_kbps = *tx;

  /* GPU metrics from plugin */
  if (config.show_gpu) {
    auto gpu_metrics = pm.get_by_prefix("gpu.");
    std::unordered_map<size_t, GpuMetrics> gpu_map;
    for (const auto& m : gpu_metrics) {
      size_t idx = 0;
      if (std::sscanf(m.key.c_str(), "gpu.%zu.", &idx) != 1) continue;

      if (m.key.find(".name") != std::string::npos && m.value.type == HMON_VAL_STRING) {
        gpu_map[idx].name = m.value.v.str;
      } else if (m.key.find(".source") != std::string::npos && m.value.type == HMON_VAL_STRING) {
        gpu_map[idx].source = m.value.v.str;
      } else if (m.key.find(".temp_c") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].temperature_c = m.value.v.f64;
      } else if (m.key.find(".clock_mhz") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].core_clock_mhz = m.value.v.f64;
      } else if (m.key.find(".usage_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].utilization_percent = m.value.v.f64;
      } else if (m.key.find(".power_w") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].power_w = m.value.v.f64;
      } else if (m.key.find(".vram_used_mib") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].memory_used_mib = m.value.v.f64;
      } else if (m.key.find(".vram_total_mib") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].memory_total_mib = m.value.v.f64;
      } else if (m.key.find(".vram_usage_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].memory_utilization_percent = m.value.v.f64;
      } else if (m.key.find(".in_use") != std::string::npos && m.value.type == HMON_VAL_BOOL) {
        gpu_map[idx].in_use = m.value.v.b != 0;
      } else if (m.key.find(".core_usage.") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
        gpu_map[idx].gpu_core_usage_percent.push_back(m.value.v.f64);
      }
    }

    for (size_t i = 0; i < gpu_map.size(); ++i) {
      if (gpu_map.count(i)) {
        snapshot.gpus.push_back(gpu_map[i]);
      }
    }
  }

  /* Docker metrics from plugin */
  auto docker_metrics = pm.get_by_prefix("docker.");
  std::unordered_map<size_t, DockerContainer> docker_map;
  for (const auto& m : docker_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "docker.%zu.", &idx) != 1) continue;

    if (m.key.find(".name") != std::string::npos && m.value.type == HMON_VAL_STRING) {
      docker_map[idx].name = m.value.v.str;
    } else if (m.key.find(".image") != std::string::npos && m.value.type == HMON_VAL_STRING) {
      docker_map[idx].image = m.value.v.str;
    } else if (m.key.find(".state") != std::string::npos && m.value.type == HMON_VAL_STRING) {
      docker_map[idx].state = m.value.v.str;
    } else if (m.key.find(".cpu_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].cpu_percent = m.value.v.f64;
    } else if (m.key.find(".mem_usage") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      docker_map[idx].mem_usage = static_cast<uint64_t>(m.value.v.i64);
    } else if (m.key.find(".mem_limit") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      docker_map[idx].mem_limit = static_cast<uint64_t>(m.value.v.i64);
    } else if (m.key.find(".mem_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].mem_percent = m.value.v.f64;
    } else if (m.key.find(".net_rx_bps") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].net_rx_bps = m.value.v.f64;
    } else if (m.key.find(".net_tx_bps") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].net_tx_bps = m.value.v.f64;
    } else if (m.key.find(".net_rx_total") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      docker_map[idx].net_rx_total = static_cast<uint64_t>(m.value.v.i64);
    } else if (m.key.find(".net_tx_total") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      docker_map[idx].net_tx_total = static_cast<uint64_t>(m.value.v.i64);
    } else if (m.key.find(".blk_read_bps") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].blk_read_bps = m.value.v.f64;
    } else if (m.key.find(".blk_write_bps") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      docker_map[idx].blk_write_bps = m.value.v.f64;
    } else if (m.key.find(".pids") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      docker_map[idx].pids_current = static_cast<int>(m.value.v.i64);
    }
  }

  for (size_t i = 0; i < docker_map.size(); ++i) {
    if (docker_map.count(i)) {
      snapshot.docker_containers.push_back(docker_map[i]);
    }
  }

  snapshot.docker_loading = snapshot.docker_containers.empty();

  /* Ports */
  auto port_metrics = pm.get_by_prefix("ports.");
  std::unordered_map<size_t, ListeningPort> port_map;
  for (const auto& m : port_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "ports.%zu.", &idx) != 1) continue;
    if (m.key.find(".port") != std::string::npos && m.value.type == HMON_VAL_INT64)
      port_map[idx].port = static_cast<uint16_t>(m.value.v.i64);
    else if (m.key.find(".proto") != std::string::npos && m.value.type == HMON_VAL_STRING)
      port_map[idx].proto = m.value.v.str;
    else if (m.key.find(".addr") != std::string::npos && m.value.type == HMON_VAL_STRING)
      port_map[idx].addr = m.value.v.str;
    else if (m.key.find(".pid") != std::string::npos && m.value.type == HMON_VAL_INT64)
      port_map[idx].pid = static_cast<int>(m.value.v.i64);
    else if (m.key.find(".process") != std::string::npos && m.value.type == HMON_VAL_STRING)
      port_map[idx].process = m.value.v.str;
  }
  for (size_t i = 0; i < port_map.size(); ++i)
    if (port_map.count(i)) snapshot.ports.push_back(port_map[i]);

  /* Systemd services */
  auto svc_metrics = pm.get_by_prefix("systemd.");
  std::unordered_map<size_t, ServiceInfo> svc_map;
  for (const auto& m : svc_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "systemd.%zu.", &idx) != 1) continue;
    if (m.key.find(".name") != std::string::npos && m.value.type == HMON_VAL_STRING)
      svc_map[idx].name = m.value.v.str;
    else if (m.key.find(".state") != std::string::npos && m.value.type == HMON_VAL_STRING)
      svc_map[idx].state = m.value.v.str;
    else if (m.key.find(".sub") != std::string::npos && m.value.type == HMON_VAL_STRING)
      svc_map[idx].sub_state = m.value.v.str;
    else if (m.key.find(".desc") != std::string::npos && m.value.type == HMON_VAL_STRING)
      svc_map[idx].description = m.value.v.str;
  }
  for (size_t i = 0; i < svc_map.size(); ++i)
    if (svc_map.count(i)) snapshot.services.push_back(svc_map[i]);

  /* Databases */
  auto db_metrics = pm.get_by_prefix("db.");
  std::unordered_map<size_t, DbInfo> db_map;
  for (const auto& m : db_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "db.%zu.", &idx) != 1) continue;
    if (m.key.find(".type") != std::string::npos && m.value.type == HMON_VAL_STRING)
      db_map[idx].type = m.value.v.str;
    else if (m.key.find(".status") != std::string::npos && m.value.type == HMON_VAL_STRING)
      db_map[idx].status = m.value.v.str;
    else if (m.key.find(".active_conns") != std::string::npos && m.value.type == HMON_VAL_INT64)
      db_map[idx].active_connections = static_cast<int>(m.value.v.i64);
    else if (m.key.find(".max_conns") != std::string::npos && m.value.type == HMON_VAL_INT64)
      db_map[idx].max_connections = static_cast<int>(m.value.v.i64);
    else if (m.key.find(".uptime") != std::string::npos && m.value.type == HMON_VAL_INT64)
      db_map[idx].uptime_seconds = m.value.v.i64;
    else if (m.key.find(".version") != std::string::npos && m.value.type == HMON_VAL_STRING)
      db_map[idx].version = m.value.v.str;
  }
  for (size_t i = 0; i < db_map.size(); ++i)
    if (db_map.count(i)) snapshot.databases.push_back(db_map[i]);

  /* Web servers */
  auto ws_metrics = pm.get_by_prefix("web.");
  std::unordered_map<size_t, WebServerInfo> ws_map;
  for (const auto& m : ws_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "web.%zu.", &idx) != 1) continue;
    if (m.key.find(".type") != std::string::npos && m.value.type == HMON_VAL_STRING)
      ws_map[idx].type = m.value.v.str;
    else if (m.key.find(".status") != std::string::npos && m.value.type == HMON_VAL_STRING)
      ws_map[idx].status = m.value.v.str;
    else if (m.key.find(".active_conns") != std::string::npos && m.value.type == HMON_VAL_INT64)
      ws_map[idx].active_connections = static_cast<int>(m.value.v.i64);
    else if (m.key.find(".rps") != std::string::npos && m.value.type == HMON_VAL_DOUBLE)
      ws_map[idx].requests_per_sec = m.value.v.f64;
    else if (m.key.find(".total_req") != std::string::npos && m.value.type == HMON_VAL_INT64)
      ws_map[idx].total_requests = m.value.v.i64;
  }
  for (size_t i = 0; i < ws_map.size(); ++i)
    if (ws_map.count(i)) snapshot.webservers.push_back(ws_map[i]);

  /* Cron jobs */
  auto cron_metrics = pm.get_by_prefix("cron.");
  std::unordered_map<size_t, CronJob> cron_map;
  for (const auto& m : cron_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "cron.%zu.", &idx) != 1) continue;
    if (m.key.find(".schedule") != std::string::npos && m.value.type == HMON_VAL_STRING)
      cron_map[idx].schedule = m.value.v.str;
    else if (m.key.find(".user") != std::string::npos && m.value.type == HMON_VAL_STRING)
      cron_map[idx].user = m.value.v.str;
    else if (m.key.find(".command") != std::string::npos && m.value.type == HMON_VAL_STRING)
      cron_map[idx].command = m.value.v.str;
    else if (m.key.find(".source") != std::string::npos && m.value.type == HMON_VAL_STRING)
      cron_map[idx].source = m.value.v.str;
  }
  for (size_t i = 0; i < cron_map.size(); ++i)
    if (cron_map.count(i)) snapshot.cron_jobs.push_back(cron_map[i]);

  return snapshot;
}

std::vector<ProcessInfo> collectProcesses(hmon::core::PluginManager& pm, size_t limit, SortMode /*sort_mode*/, int /*lock_pid*/) {
  std::vector<ProcessInfo> processes;

  auto proc_metrics = pm.get_by_prefix("proc.");
  std::unordered_map<size_t, ProcessInfo> proc_map;
  for (const auto& m : proc_metrics) {
    size_t idx = 0;
    if (std::sscanf(m.key.c_str(), "proc.%zu.", &idx) != 1) continue;

    if (m.key.find(".pid") != std::string::npos && m.value.type == HMON_VAL_INT64) {
      proc_map[idx].pid = static_cast<int>(m.value.v.i64);
    } else if (m.key.find(".cpu_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      proc_map[idx].cpu_percent = m.value.v.f64;
    } else if (m.key.find(".mem_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      proc_map[idx].mem_percent = m.value.v.f64;
    } else if (m.key.find(".gpu_pct") != std::string::npos && m.value.type == HMON_VAL_DOUBLE) {
      proc_map[idx].gpu_percent = m.value.v.f64;
    } else if (m.key.find(".command") != std::string::npos && m.value.type == HMON_VAL_STRING) {
      proc_map[idx].command = m.value.v.str;
    }
  }

  for (size_t i = 0; i < proc_map.size() && processes.size() < limit; ++i) {
    if (proc_map.count(i)) {
      processes.push_back(proc_map[i]);
    }
  }

  return processes;
}

void updateHistory(MetricsHistory* history, const Snapshot& snapshot, size_t max_points,
                   const std::optional<double>& disk_busy_percent) {
  if (!history || max_points == 0) return;

  auto appendValue = [&](std::vector<double>& series, const std::optional<double>& value) {
    const double next_value = std::max(0.0, std::min(100.0, value.value_or(series.empty() ? 0.0 : series.back())));
    series.push_back(next_value);
    if (series.size() > max_points) {
      series.erase(series.begin(), series.begin() + static_cast<std::ptrdiff_t>(series.size() - max_points));
    }
  };

  appendValue(history->cpu_usage, snapshot.cpu.usage_percent);
  appendValue(history->cpu_temp, snapshot.cpu.temperature_c);
  
  if (snapshot.ram.total_kb && snapshot.ram.available_kb && *snapshot.ram.total_kb > 0) {
    const double ram_pct = 100.0 * (static_cast<double>(*snapshot.ram.total_kb) - 
                                    static_cast<double>(*snapshot.ram.available_kb)) / 
                           static_cast<double>(*snapshot.ram.total_kb);
    appendValue(history->ram_usage, ram_pct);
  } else {
    appendValue(history->ram_usage, std::nullopt);
  }

  if (!snapshot.gpus.empty()) {
    const auto& gpu = snapshot.gpus[pickDisplayGpuIndex(snapshot.gpus)];
    appendValue(history->gpu_usage, gpu.utilization_percent);
    if (gpu.memory_utilization_percent) {
      appendValue(history->gpu_vram_usage, gpu.memory_utilization_percent);
    } else if (gpu.memory_used_mib && gpu.memory_total_mib && *gpu.memory_total_mib > 0) {
      appendValue(history->gpu_vram_usage, 100.0 * (*gpu.memory_used_mib) / (*gpu.memory_total_mib));
    } else {
      appendValue(history->gpu_vram_usage, std::nullopt);
    }
  } else {
    appendValue(history->gpu_usage, std::nullopt);
    appendValue(history->gpu_vram_usage, std::nullopt);
  }

  if (disk_busy_percent) {
    appendValue(history->disk_usage, disk_busy_percent);
  } else if (snapshot.disk.total_bytes && snapshot.disk.free_bytes && *snapshot.disk.total_bytes > 0) {
    const double disk_pct = 100.0 * static_cast<double>(*snapshot.disk.total_bytes - *snapshot.disk.free_bytes) /
                            static_cast<double>(*snapshot.disk.total_bytes);
    appendValue(history->disk_usage, disk_pct);
  } else {
    appendValue(history->disk_usage, std::nullopt);
  }
}

void renderSnapshot(const Snapshot& snapshot, const MetricsHistory& history,
                    const std::vector<ProcessInfo>& processes, const std::string& host,
                    const Config& config, int refresh_interval_ms) {
  erase();

  int rows = 0, cols = 0;
  getmaxyx(stdscr, rows, cols);

  if (rows < 18 || cols < 80) {
    attron(A_BOLD);
    mvaddnstr(2, 2, "Terminal too small. Resize to at least 80x18.", std::max(0, cols - 4));
    mvaddnstr(3, 2, "Press q to quit.", std::max(0, cols - 4));
    attroff(A_BOLD);
    refresh();
    return;
  }

  if (config.zen_mode) {
    renderZenMode(stdscr, snapshot, config, processes);
    doupdate();
    return;
  }

  const std::string logo = " hmon " + std::string(version::kCurrent) + " ";
  const std::string status = "Host: " + host + "  |  Refresh: " +
                             (refresh_interval_ms >= 1000 ? std::to_string(refresh_interval_ms / 1000) + "s" :
                                                            std::to_string(refresh_interval_ms) + "ms");
  const std::string time_str = currentTimestamp();

  attron(A_BOLD);
  if (has_colors()) {
    attron(COLOR_PAIR(4));
    attron(A_REVERSE);
  }
  mvaddnstr(0, 0, logo.c_str(), static_cast<int>(logo.size()));
  if (has_colors()) {
    attroff(A_REVERSE);
    attroff(COLOR_PAIR(4));
  }
  attroff(A_BOLD);

  mvaddnstr(0, static_cast<int>(logo.size()) + 1, status.c_str(), cols - static_cast<int>(logo.size()) - static_cast<int>(time_str.size()) - 2);

  if (has_colors()) {
    attron(COLOR_PAIR(7));
  }
  mvaddnstr(0, cols - static_cast<int>(time_str.size()) - 1, time_str.c_str(), static_cast<int>(time_str.size()));
  if (has_colors()) {
    attroff(COLOR_PAIR(7));
  }

  for (int x = 0; x < cols; ++x) {
    mvaddch(1, x, ACS_HLINE);
  }

  std::string shortcuts = " q:Quit  z:Zen  s:Sort  l:Lock  u:Unlock  +/-:Speed  r:Refresh  ?:Help ";
  attron(A_REVERSE);
  mvaddnstr(rows - 1, 0, shortcuts.c_str(), cols);
  attroff(A_REVERSE);

  const int top = 2;
  const int gap = 1;
  const int margin = 1;
  const int content_w = cols - 2 * margin - gap;
  const int left_w = content_w / 2;
  const int right_w = content_w - left_w;
  const int x_left = margin;
  const int x_right = x_left + left_w + gap;

  const int content_h = rows - top - 2;
  const int history_min_h = std::max(6, estimateHistoryRows() / 2);
  const int min_panel_h = 4;
  const int min_stack_h = min_panel_h * 3 + gap * 2;
  const int left_pref_top_h = estimateCpuRows(snapshot) + 1;
  const int left_pref_bottom_h = estimateRamRows() + estimateNetworkRows() + gap + 1;
  const int right_pref_top_h = config.show_gpu ? estimateGpuRows(snapshot) + 1 : 0;
  const int right_pref_bottom_h = estimateDiskRows() + 1;
  const int pref_stack_h = std::max(left_pref_top_h + gap + left_pref_bottom_h,
                                    right_pref_top_h + gap + right_pref_bottom_h);
  const int stack_h = std::min(content_h, std::max(min_stack_h, pref_stack_h));
  const int remaining_h = content_h - stack_h;
  const bool has_history_panel = config.show_history && remaining_h >= (history_min_h + gap);
  const int history_h = has_history_panel ? (remaining_h - gap) : 0;

  int cpu_h = min_panel_h, ram_h = min_panel_h, gpu_h = min_panel_h, disk_h = min_panel_h;
  splitColumnHeights(stack_h, left_pref_top_h, left_pref_bottom_h, gap, &cpu_h, &ram_h);
  splitColumnHeights(stack_h, right_pref_top_h, right_pref_bottom_h, gap, &gpu_h, &disk_h);

  const Rect cpu_rect{top, x_left, cpu_h, left_w};
  const Rect ram_rect{top + cpu_h + gap, x_left, ram_h, left_w};
  const Rect gpu_rect{top, x_right, gpu_h, right_w};
  const Rect disk_rect{top + gpu_h + gap, x_right, disk_h, right_w};
  const Rect history_rect{top + stack_h + gap, margin, history_h, cols - 2 * margin};
  const Rect net_rect{top + ram_h + gap, x_left, std::max(min_panel_h, content_h - cpu_h - ram_h - gap * 2), left_w};

  WINDOW* cpu_panel = createPanel(cpu_rect, "CPU");
  WINDOW* ram_panel = createPanel(ram_rect, "RAM");
  WINDOW* gpu_panel = config.show_gpu ? createPanel(gpu_rect, "GPU") : nullptr;
  WINDOW* net_panel = createPanel(net_rect, "NETWORK");
  WINDOW* disk_panel = createPanel(disk_rect, "DISK");
  WINDOW* history_panel = has_history_panel ? createPanel(history_rect, "ACTIVITY HISTORY") : nullptr;

  renderCpuPanel(cpu_panel, snapshot);
  renderNetworkPanel(net_panel, snapshot);
  renderRamPanel(ram_panel, snapshot);
  if (config.show_gpu) {
    renderGpuPanel(gpu_panel, snapshot);
  }
  renderDiskPanel(disk_panel, snapshot);
  if (has_history_panel) {
    renderHistoryPanel(history_panel, history, processes, config.selected_pid, config.lock_pid,
                       config.show_selection_highlight, config.sort_mode);
  }

  wnoutrefresh(stdscr);

  if (cpu_panel) { wnoutrefresh(cpu_panel); delwin(cpu_panel); }
  if (ram_panel) { wnoutrefresh(ram_panel); delwin(ram_panel); }
  if (gpu_panel) { wnoutrefresh(gpu_panel); delwin(gpu_panel); }
  if (net_panel) { wnoutrefresh(net_panel); delwin(net_panel); }
  if (disk_panel) { wnoutrefresh(disk_panel); delwin(disk_panel); }
  if (history_panel) { wnoutrefresh(history_panel); delwin(history_panel); }

  doupdate();
}

int main(int argc, char* argv[]) {
  Config config = parseArgs(argc, argv);

  if (config.cli_error) {
    std::cerr << *config.cli_error << "\n\n";
    printHelp(argv[0]);
    return 1;
  }

  if (config.show_help) {
    printHelp(argv[0]);
    return 0;
  }

  if (config.show_version) {
    printVersion();
    return 0;
  }

  std::setlocale(LC_ALL, "");

  /* Load plugins BEFORE ncurses so errors don't corrupt the display */
  hmon::core::PluginManager pm;

  const char* env_plugin_dir = std::getenv("HMON_PLUGIN_DIR");
  if (env_plugin_dir) {
    pm.load_directory(env_plugin_dir);
  } else {
    /* Try build directory first (for development), then install path */
    pm.load_directory("./");
    if (pm.plugin_count() == 0) {
      pm.load_directory(HMON_PLUGIN_DIR);
    }
  }

  if (pm.plugin_count() == 0) {
    std::cerr << "hmon: no plugins found. Set HMON_PLUGIN_DIR or run from build directory.\n";
    return 1;
  }

  if (pm.init_all() != 0) {
    std::cerr << "hmon: failed to initialise one or more plugins.\n";
    return 1;
  }

  initscr();
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  timeout(config.refresh_interval_ms);

  if (has_colors() && config.show_colors) {
    start_color();
    use_default_colors();
    if (can_change_color() && COLORS >= 32) {
      constexpr short kSoftGreen = 20, kSoftAmber = 21, kSoftRose = 22;
      constexpr short kSoftCyan = 23, kSoftLavender = 24, kSoftBlue = 25, kSoftGray = 26;

      init_color(kSoftGreen, 420, 760, 560);
      init_color(kSoftAmber, 780, 700, 430);
      init_color(kSoftRose, 760, 480, 520);
      init_color(kSoftCyan, 460, 720, 760);
      init_color(kSoftLavender, 680, 560, 760);
      init_color(kSoftBlue, 430, 560, 760);
      init_color(kSoftGray, 600, 600, 620);

      init_pair(1, kSoftGreen, -1);
      init_pair(2, kSoftAmber, -1);
      init_pair(3, kSoftRose, -1);
      init_pair(4, kSoftCyan, -1);
      init_pair(5, kSoftLavender, -1);
      init_pair(6, kSoftBlue, -1);
      init_pair(7, kSoftGray, -1);
    } else {
      init_pair(1, COLOR_GREEN, -1);
      init_pair(2, COLOR_YELLOW, -1);
      init_pair(3, COLOR_RED, -1);
      init_pair(4, COLOR_CYAN, -1);
      init_pair(5, COLOR_MAGENTA, -1);
      init_pair(6, COLOR_BLUE, -1);
      init_pair(7, COLOR_WHITE, -1);
    }
  }

  const std::string host = hostName();
  MetricsHistory history;
  int refresh_interval_ms = config.refresh_interval_ms;
  bool show_help_overlay = false;

  pm.collect_all();
  Snapshot snapshot = collectSnapshot(pm, config);
  std::vector<ProcessInfo> processes = collectProcesses(pm, config.top_processes, config.sort_mode, config.lock_pid);
  syncSelection(processes, &config);
  updateHistory(&history, snapshot, config.history_points, computeRootDiskBusyPercent());
  renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);

  while (true) {
    const int ch = getch();

    if (ch == 'q' || ch == 'Q') {
      break;
    }

    if (ch == '?') {
      show_help_overlay = !show_help_overlay;
      if (show_help_overlay) {
        timeout(-1);
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int overlay_h = std::min(16, rows - 4);
        int overlay_w = std::min(54, cols - 4);
        int start_y = (rows - overlay_h) / 2;
        int start_x = (cols - overlay_w) / 2;

        WINDOW* help_win = newwin(overlay_h, overlay_w, start_y, start_x);
        drawHelpOverlay(help_win, config);
        delwin(help_win);
        doupdate();
      } else {
        timeout(refresh_interval_ms);
        renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      }
      continue;
    }

    if (show_help_overlay && ch != ERR) {
      show_help_overlay = false;
      timeout(refresh_interval_ms);
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == KEY_RESIZE) {
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == 'z' || ch == 'Z') {
      config.zen_mode = !config.zen_mode;
      if (config.zen_mode) {
        pm.control("docker", "docker.enable", 1);
        config.zen_docker_scroll = 0;
        config.zen_ports_scroll = 0;
        config.zen_services_scroll = 0;
        config.zen_focus = ZenFocus::kNone;
      } else {
        pm.control("docker", "docker.enable", 0);
      }
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (config.zen_mode && ch == 9) { /* Tab — cycle focus */
      std::vector<ZenFocus> available;
      if (!snapshot.docker_containers.empty()) available.push_back(ZenFocus::kDocker);
      if (!snapshot.ports.empty()) available.push_back(ZenFocus::kPorts);
      if (!snapshot.services.empty()) available.push_back(ZenFocus::kServices);
      if (available.empty()) { config.zen_focus = ZenFocus::kNone; }
      else {
        auto it = std::find(available.begin(), available.end(), config.zen_focus);
        if (it == available.end() || it + 1 == available.end()) config.zen_focus = available.front();
        else config.zen_focus = *(it + 1);
      }
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (config.zen_mode && (ch == 'j' || ch == 'J' || ch == KEY_DOWN)) {
      if (config.zen_focus == ZenFocus::kDocker && !snapshot.docker_containers.empty()) {
        int max_show = 4;
        int total = static_cast<int>(snapshot.docker_containers.size());
        config.zen_docker_scroll = std::min(config.zen_docker_scroll + 1, std::max(0, total - max_show));
      } else if (config.zen_focus == ZenFocus::kPorts && !snapshot.ports.empty()) {
        int max_show = 6;
        int total = static_cast<int>(snapshot.ports.size());
        config.zen_ports_scroll = std::min(config.zen_ports_scroll + 1, std::max(0, total - max_show));
      } else if (config.zen_focus == ZenFocus::kServices && !snapshot.services.empty()) {
        int max_show = 6;
        int total = static_cast<int>(snapshot.services.size());
        config.zen_services_scroll = std::min(config.zen_services_scroll + 1, std::max(0, total - max_show));
      }
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (config.zen_mode && (ch == 'k' || ch == 'K' || ch == KEY_UP)) {
      config.zen_docker_scroll = std::max(0, config.zen_docker_scroll - 1);
      config.zen_ports_scroll = std::max(0, config.zen_ports_scroll - 1);
      config.zen_services_scroll = std::max(0, config.zen_services_scroll - 1);
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == 's' || ch == 'S') {
      switch (config.sort_mode) {
        case SortMode::kCpu: config.sort_mode = SortMode::kMem; break;
        case SortMode::kMem: config.sort_mode = SortMode::kGpu; break;
        case SortMode::kGpu: config.sort_mode = SortMode::kPid; break;
        case SortMode::kPid: config.sort_mode = SortMode::kCpu; break;
      }
      processes = collectProcesses(pm, config.top_processes, config.sort_mode, config.lock_pid);
      syncSelection(processes, &config);
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == KEY_UP || ch == 'k' || ch == 'K') {
      config.show_selection_highlight = true;
      moveSelection(processes, &config, -1);
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == KEY_DOWN || ch == 'j' || ch == 'J') {
      config.show_selection_highlight = true;
      moveSelection(processes, &config, 1);
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == 'l' || ch == 'L') {
      if (config.selected_pid > 0) {
        if (config.lock_pid == config.selected_pid) {
          config.lock_pid = -1;
        } else {
          config.lock_pid = config.selected_pid;
        }
      }
      config.show_selection_highlight = false;
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == 'u' || ch == 'U') {
      config.lock_pid = -1;
      config.show_selection_highlight = false;
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch >= '1' && ch <= '9') {
      int idx = ch - '1';
      if (idx < static_cast<int>(processes.size())) {
        config.selected_pid = processes[static_cast<size_t>(idx)].pid;
        config.show_selection_highlight = true;
        renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      }
      continue;
    }

    if (ch == 'r' || ch == 'R') {
      pm.collect_all();
      snapshot = collectSnapshot(pm, config);
      processes = collectProcesses(pm, config.top_processes, config.sort_mode, config.lock_pid);
      syncSelection(processes, &config);
      updateHistory(&history, snapshot, config.history_points, computeRootDiskBusyPercent());
      renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      continue;
    }

    if (ch == '+' || ch == '=') {
      if (refresh_interval_ms > 100) {
        refresh_interval_ms = std::max(100, refresh_interval_ms - 100);
        timeout(refresh_interval_ms);
        renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      }
      continue;
    }

    if (ch == '-' || ch == '_') {
      if (refresh_interval_ms < 10000) {
        refresh_interval_ms = std::min(10000, refresh_interval_ms + 100);
        timeout(refresh_interval_ms);
        renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
      }
      continue;
    }

    if (ch != ERR) {
      continue;
    }

    pm.collect_all();
    snapshot = collectSnapshot(pm, config);
    processes = collectProcesses(pm, config.top_processes, config.sort_mode, config.lock_pid);
    syncSelection(processes, &config);
    updateHistory(&history, snapshot, config.history_points, computeRootDiskBusyPercent());
    renderSnapshot(snapshot, history, processes, host, config, refresh_interval_ms);
  }

  pm.destroy_all();
  endwin();
  return 0;
}
