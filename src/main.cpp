#include <ncurses.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "metrics/cpu.hpp"
#include "metrics/gpu.hpp"
#include "metrics/linux_utils.hpp"
#include "metrics/system.hpp"
#include "metrics/types.hpp"

struct Rect {
  int y;
  int x;
  int h;
  int w;
};

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

std::string formatCpuTopology(const CpuMetrics& cpu) {
  if (!cpu.total_cores && !cpu.total_threads) {
    return "N/A";
  }
  const std::string cores = cpu.total_cores ? std::to_string(*cpu.total_cores) : "N/A";
  const std::string threads = cpu.total_threads ? std::to_string(*cpu.total_threads) : "N/A";
  return cores + "C / " + threads + "T";
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

std::string formatGpuVramUsage(const GpuMetrics& gpu) {
  if (!gpu.memory_used_mib || !gpu.memory_total_mib) {
    return "N/A";
  }
  return formatMibOrGib(gpu.memory_used_mib) + " / " + formatMibOrGib(gpu.memory_total_mib);
}

bool gpuHasTelemetry(const GpuMetrics& gpu) {
  return gpu.temperature_c || gpu.core_clock_mhz || gpu.utilization_percent || gpu.power_w ||
         gpu.memory_used_mib || gpu.memory_total_mib || gpu.memory_utilization_percent;
}

bool gpuLooksIntel(const GpuMetrics& gpu) {
  const std::string lower_name = linux_utils::toLower(gpu.name);
  const std::string lower_source = linux_utils::toLower(gpu.source);
  return lower_name.find("intel") != std::string::npos || lower_source.find("intel") != std::string::npos ||
         lower_source.find("i915") != std::string::npos || lower_source.find("xe") != std::string::npos;
}

bool gpuLooksRadeon(const GpuMetrics& gpu) {
  const std::string lower_name = linux_utils::toLower(gpu.name);
  const std::string lower_source = linux_utils::toLower(gpu.source);
  return lower_source.find("radeon") != std::string::npos || lower_name.find("radeon") != std::string::npos;
}

bool anyGpuHasTelemetry(const std::vector<GpuMetrics>& gpus) {
  return std::any_of(gpus.begin(), gpus.end(), [](const GpuMetrics& gpu) { return gpuHasTelemetry(gpu); });
}

bool gpuIsRelevantForSummary(const GpuMetrics& gpu) {
  return gpuHasTelemetry(gpu) || (gpu.in_use && *gpu.in_use);
}

size_t pickDisplayGpuIndex(const std::vector<GpuMetrics>& gpus) {
  if (gpus.empty()) {
    return 0;
  }

  size_t first_with_telemetry = gpus.size();
  size_t intel_with_telemetry = gpus.size();
  size_t first_intel = gpus.size();

  for (size_t i = 0; i < gpus.size(); ++i) {
    const bool is_intel = gpuLooksIntel(gpus[i]);
    if (is_intel && first_intel == gpus.size()) {
      first_intel = i;
    }

    if (gpuHasTelemetry(gpus[i])) {
      if (first_with_telemetry == gpus.size()) {
        first_with_telemetry = i;
      }
      if (is_intel && intel_with_telemetry == gpus.size()) {
        intel_with_telemetry = i;
      }
    }
  }

  if (gpuLooksRadeon(gpus.front()) && !gpuHasTelemetry(gpus.front())) {
    if (intel_with_telemetry != gpus.size()) {
      return intel_with_telemetry;
    }
    if (first_with_telemetry != gpus.size()) {
      return first_with_telemetry;
    }
    if (first_intel != gpus.size()) {
      return first_intel;
    }
  }

  if (first_with_telemetry != gpus.size()) {
    return first_with_telemetry;
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

size_t countAdditionalRelevantGpus(const std::vector<GpuMetrics>& gpus, size_t selected_gpu_index) {
  if (selected_gpu_index >= gpus.size()) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (i == selected_gpu_index) {
      continue;
    }
    if (gpuIsRelevantForSummary(gpus[i])) {
      ++count;
    }
  }
  return count;
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

int colorPairForBarFillPosition(int filled_index, int filled_count) {
  if (filled_count <= 0) {
    return 1;
  }
  const double progress = 100.0 * static_cast<double>(filled_index + 1) / static_cast<double>(filled_count);
  return colorPairForPercent(progress);
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

void drawPipeBar(WINDOW* win, int row, const std::string& label, double percent , int type = 0) {
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
  const std::string prefix = label + "[";
  const int suffix_width = static_cast<int>(value_text.size()) + 2;  // "] " + value
  int inner_width = max_x - 4 - static_cast<int>(prefix.size()) - suffix_width;
  if (inner_width < 8) {
    inner_width = 8;
  }
  const int filled = static_cast<int>(std::round((clamped / 100.0) * static_cast<double>(inner_width)));

  const int start_col = 2;
  mvwaddnstr(win, row, start_col, prefix.c_str(), max_x - 4);
  const int bar_col = start_col + static_cast<int>(prefix.size());
  const int capped_filled = std::max(0, std::min(inner_width, filled));

  for (int i = 0; i < inner_width; ++i) {
    if (i < capped_filled) {
      if (has_colors()) {
        wattron(win, COLOR_PAIR(colorPairForBarFillPosition(i, capped_filled)));
      }
      wattron(win, A_DIM);
      mvwaddch(win, row, bar_col + i, '|');
      wattroff(win, A_DIM);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(colorPairForBarFillPosition(i, capped_filled)));
      }
    } else {
      if (has_colors()) {
        wattron(win, COLOR_PAIR(7));
      }
      wattron(win, A_DIM);
      mvwaddch(win, row, bar_col + i, ' ');
      wattroff(win, A_DIM);
      if (has_colors()) {
        wattroff(win, COLOR_PAIR(7));
      }
    }
  }

  mvwaddch(win, row, bar_col + inner_width, ']');
  if (has_colors()) {
    wattron(win, COLOR_PAIR(2));
  }
  mvwaddnstr(win, row, bar_col + inner_width + 1, (" " + value_text).c_str(), max_x - 2 - (bar_col + inner_width + 1));
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(2));
  }
}

WINDOW* createPanel(const Rect& rect, const std::string& title) {
  if (rect.h < 4 || rect.w < 20) {
    return nullptr;
  }
  WINDOW* panel = newwin(rect.h, rect.w, rect.y, rect.x);
  box(panel, 0, 0);
  mvwprintw(panel, 0, 2, " %s ", title.c_str());
  return panel;
}

struct MetricsHistory {
  std::vector<double> cpu_usage;
  std::vector<double> cpu_temp;
  std::vector<double> ram_usage;
  std::vector<double> gpu_usage;
  std::vector<double> gpu_vram_usage;
  std::vector<double> disk_usage;
};

struct ProcessInfo {
  int pid = 0;
  double cpu_percent = 0.0;
  double mem_percent = 0.0;
  std::string command;
};

struct BrailleCanvas {
  int width = 0;   // terminal cells (x)
  int height = 0;  // terminal cells (y)
  std::vector<uint16_t> cells;  // dot masks, size = width * height
};

constexpr int kTargetFps = 1;
constexpr int kFrameIntervalMs = 1000 / kTargetFps;
constexpr uint16_t kDirUp = 0x01;
constexpr uint16_t kDirDown = 0x02;
constexpr uint16_t kDirLeft = 0x04;
constexpr uint16_t kDirRight = 0x08;
constexpr uint16_t kDirPoint = 0x10;

double clampPercent(double value) {
  return std::max(0.0, std::min(100.0, value));
}

std::optional<std::pair<unsigned int, unsigned int>> rootDeviceNumbers() {
  struct stat stat_info {};
  if (stat("/", &stat_info) != 0) {
    return std::nullopt;
  }
  return std::make_pair(static_cast<unsigned int>(major(stat_info.st_dev)),
                        static_cast<unsigned int>(minor(stat_info.st_dev)));
}

std::optional<unsigned long long> readDiskIoTimeMsForDevice(unsigned int target_major, unsigned int target_minor) {
  std::ifstream diskstats("/proc/diskstats");
  if (!diskstats) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(diskstats, line)) {
    std::istringstream line_stream(line);
    unsigned int major_num = 0;
    unsigned int minor_num = 0;
    std::string device_name;
    if (!(line_stream >> major_num >> minor_num >> device_name)) {
      continue;
    }
    if (major_num != target_major || minor_num != target_minor) {
      continue;
    }

    std::vector<unsigned long long> fields;
    unsigned long long value = 0;
    while (line_stream >> value) {
      fields.push_back(value);
    }
    if (fields.size() <= 9) {
      return std::nullopt;
    }
    return fields[9];
  }

  return std::nullopt;
}

std::optional<double> computeRootDiskBusyPercent() {
  static std::optional<std::pair<unsigned int, unsigned int>> root_numbers = rootDeviceNumbers();
  static std::optional<unsigned long long> previous_io_ms;
  static std::chrono::steady_clock::time_point previous_time;

  if (!root_numbers) {
    return std::nullopt;
  }

  const auto current_io_ms = readDiskIoTimeMsForDevice(root_numbers->first, root_numbers->second);
  const auto now = std::chrono::steady_clock::now();
  if (!current_io_ms) {
    return std::nullopt;
  }

  if (!previous_io_ms) {
    previous_io_ms = current_io_ms;
    previous_time = now;
    return std::nullopt;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous_time);
  const long long delta_ms = static_cast<long long>(*current_io_ms) - static_cast<long long>(*previous_io_ms);
  previous_io_ms = current_io_ms;
  previous_time = now;

  if (elapsed.count() <= 0 || delta_ms < 0) {
    return std::nullopt;
  }

  const double busy = 100.0 * static_cast<double>(delta_ms) / static_cast<double>(elapsed.count());
  return clampPercent(busy);
}

std::optional<double> computeRamUsagePercent(const Snapshot& snapshot) {
  if (!snapshot.ram.total_kb || !snapshot.ram.available_kb || *snapshot.ram.total_kb <= 0) {
    return std::nullopt;
  }
  const long long total_kb = *snapshot.ram.total_kb;
  const long long available_kb = std::max(0LL, *snapshot.ram.available_kb);
  const long long used_kb = std::max(0LL, total_kb - available_kb);
  return 100.0 * static_cast<double>(used_kb) / static_cast<double>(total_kb);
}

std::optional<double> computeGpuUsagePercent(const Snapshot& snapshot) {
  if (snapshot.gpus.empty()) {
    return std::nullopt;
  }
  return snapshot.gpus[pickDisplayGpuIndex(snapshot.gpus)].utilization_percent;
}

std::optional<double> computeGpuVramUsagePercent(const Snapshot& snapshot) {
  if (snapshot.gpus.empty()) {
    return std::nullopt;
  }
  const auto& gpu = snapshot.gpus[pickDisplayGpuIndex(snapshot.gpus)];
  if (gpu.memory_utilization_percent) {
    return gpu.memory_utilization_percent;
  }
  if (gpu.memory_used_mib && gpu.memory_total_mib && *gpu.memory_total_mib > 0.0) {
    return 100.0 * (*gpu.memory_used_mib) / (*gpu.memory_total_mib);
  }
  return std::nullopt;
}

std::optional<double> computeDiskUsagePercent(const Snapshot& snapshot) {
  if (!snapshot.disk.total_bytes || !snapshot.disk.free_bytes || *snapshot.disk.total_bytes == 0) {
    return std::nullopt;
  }
  const auto total = *snapshot.disk.total_bytes;
  const auto free = std::min(*snapshot.disk.free_bytes, total);
  const auto used = total - free;
  return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}

void appendHistoryValue(std::vector<double>* series, const std::optional<double>& value, size_t max_points) {
  if (!series) {
    return;
  }
  const double next_value = clampPercent(value.value_or(series->empty() ? 0.0 : series->back()));
  series->push_back(next_value);
  if (series->size() > max_points) {
    const size_t remove_count = series->size() - max_points;
    series->erase(series->begin(), series->begin() + static_cast<std::ptrdiff_t>(remove_count));
  }
}

void updateHistory(MetricsHistory* history, const Snapshot& snapshot, size_t max_points,
                   const std::optional<double>& disk_override_percent) {
  if (!history || max_points == 0) {
    return;
  }
  appendHistoryValue(&history->cpu_usage, snapshot.cpu.usage_percent, max_points);
  appendHistoryValue(&history->cpu_temp, snapshot.cpu.temperature_c, max_points);
  appendHistoryValue(&history->ram_usage, computeRamUsagePercent(snapshot), max_points);
  appendHistoryValue(&history->gpu_usage, computeGpuUsagePercent(snapshot), max_points);
  appendHistoryValue(&history->gpu_vram_usage, computeGpuVramUsagePercent(snapshot), max_points);
  if (disk_override_percent) {
    appendHistoryValue(&history->disk_usage, disk_override_percent, max_points);
  } else {
    appendHistoryValue(&history->disk_usage, computeDiskUsagePercent(snapshot), max_points);
  }
}

bool isInsideCanvas(const BrailleCanvas* canvas, int x, int y) {
  if (!canvas) {
    return false;
  }
  return x >= 0 && y >= 0 && x < canvas->width && y < canvas->height;
}

void addCanvasMask(BrailleCanvas* canvas, int x, int y, uint16_t mask) {
  if (!isInsideCanvas(canvas, x, y)) {
    return;
  }
  const size_t index = static_cast<size_t>(y * canvas->width + x);
  canvas->cells[index] |= mask;
}

void connectCanvasCells(BrailleCanvas* canvas, int x0, int y0, int x1, int y1) {
  if (!isInsideCanvas(canvas, x0, y0) || !isInsideCanvas(canvas, x1, y1)) {
    return;
  }
  if (x1 == x0 + 1 && y1 == y0) {
    addCanvasMask(canvas, x0, y0, kDirRight);
    addCanvasMask(canvas, x1, y1, kDirLeft);
    return;
  }
  if (x1 == x0 - 1 && y1 == y0) {
    addCanvasMask(canvas, x0, y0, kDirLeft);
    addCanvasMask(canvas, x1, y1, kDirRight);
    return;
  }
  if (x1 == x0 && y1 == y0 + 1) {
    addCanvasMask(canvas, x0, y0, kDirDown);
    addCanvasMask(canvas, x1, y1, kDirUp);
    return;
  }
  if (x1 == x0 && y1 == y0 - 1) {
    addCanvasMask(canvas, x0, y0, kDirUp);
    addCanvasMask(canvas, x1, y1, kDirDown);
    return;
  }
}

std::vector<ProcessInfo> collectTopProcesses(size_t limit) {
  std::vector<ProcessInfo> processes;
  if (limit == 0) {
    return processes;
  }

  const std::string output =
      linux_utils::runCommand("ps -eo pid,%cpu,%mem,args --sort=-%cpu --no-headers 2>/dev/null");
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line) && processes.size() < limit) {
    std::istringstream line_stream(line);
    ProcessInfo process;
    if (!(line_stream >> process.pid >> process.cpu_percent >> process.mem_percent)) {
      continue;
    }

    std::string command;
    std::getline(line_stream, command);
    process.command = linux_utils::trim(command);
    if (process.command.empty()) {
      process.command = "<unknown>";
    }
    processes.push_back(process);
  }

  return processes;
}

BrailleCanvas createBrailleCanvas(int width, int height) {
  BrailleCanvas canvas;
  canvas.width = std::max(0, width);
  canvas.height = std::max(0, height);
  canvas.cells.assign(static_cast<size_t>(canvas.width * canvas.height), 0);
  return canvas;
}

void rasterizeBrailleLine(BrailleCanvas* canvas, int x0, int y0, int x1, int y1) {
  if (!canvas || !isInsideCanvas(canvas, x0, y0) || !isInsideCanvas(canvas, x1, y1)) {
    return;
  }
  int x = x0;
  int y = y0;
  addCanvasMask(canvas, x, y, kDirPoint);
  const int dx = std::abs(x1 - x0);
  const int dy = std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1;
  const int sy = y0 < y1 ? 1 : -1;
  int error = dx - dy;

  while (true) {
    if (x == x1 && y == y1) {
      break;
    }
    const int prev_x = x;
    const int prev_y = y;
    const int twice_error = error * 2;
    bool moved_x = false;
    bool moved_y = false;
    if (twice_error > -dy) {
      error -= dy;
      x += sx;
      moved_x = true;
    }
    if (twice_error < dx) {
      error += dx;
      y += sy;
      moved_y = true;
    }

    if (moved_x && moved_y) {
      // Render diagonals as connected elbows in the character grid.
      const int elbow_x = x;
      const int elbow_y = prev_y;
      connectCanvasCells(canvas, prev_x, prev_y, elbow_x, elbow_y);
      connectCanvasCells(canvas, elbow_x, elbow_y, x, y);
    } else {
      connectCanvasCells(canvas, prev_x, prev_y, x, y);
    }
    addCanvasMask(canvas, x, y, kDirPoint);
  }
}

void plotBrailleSeries(BrailleCanvas* canvas, const std::vector<double>& values,
                       double min_value, double max_value) {
  if (!canvas || canvas->width <= 0 || canvas->height <= 0 || values.empty()) {
    return;
  }
  if (max_value <= min_value) {
    return;
  }

  const int graph_w = canvas->width;
  const int graph_h = canvas->height;
  if (graph_w <= 0 || graph_h <= 0) {
    return;
  }

  const size_t sample_count = std::min(values.size(), static_cast<size_t>(graph_w));
  const size_t start_index = values.size() - sample_count;
  if (sample_count == 0) {
    return;
  }

  auto valueToPixelYTop = [&](double value) {
    const double clamped = std::max(min_value, std::min(max_value, value));
    const double normalized = (clamped - min_value) / (max_value - min_value);  // 0..1 bottom-origin
    const int y_from_bottom = static_cast<int>(std::lround(normalized * static_cast<double>(graph_h - 1)));
    const int y_top = (graph_h - 1) - y_from_bottom;
    return std::max(0, std::min(graph_h - 1, y_top));
  };

  auto sampleToPixelX = [&](size_t sample_index) {
    if (sample_count <= 1) {
      return 0;
    }
    return static_cast<int>((sample_index * static_cast<size_t>(graph_w - 1)) / (sample_count - 1));
  };

  if (sample_count == 1) {
    addCanvasMask(canvas, sampleToPixelX(0), valueToPixelYTop(values[start_index]), kDirPoint);
    return;
  }

  for (size_t i = 0; i + 1 < sample_count; ++i) {
    const int px0 = sampleToPixelX(i);
    const int px1 = sampleToPixelX(i + 1);
    const int py0 = valueToPixelYTop(values[start_index + i]);
    const int py1 = valueToPixelYTop(values[start_index + i + 1]);
    rasterizeBrailleLine(canvas, px0, py0, px1, py1);
  }

  const int last_px = sampleToPixelX(sample_count - 1);
  const int last_py = valueToPixelYTop(values.back());
  addCanvasMask(canvas, last_px, last_py, kDirPoint);
}

void drawBrailleLayer(WINDOW* win, const BrailleCanvas& canvas, int top, int left, int color_pair) {
  if (!win || canvas.width <= 0 || canvas.height <= 0 || canvas.cells.empty()) {
    return;
  }

  if (has_colors()) {
    wattron(win, COLOR_PAIR(color_pair));
  }
  const auto glyphForMask = [](uint16_t mask) -> wchar_t {
    const uint16_t dirs = mask & static_cast<uint16_t>(kDirUp | kDirDown | kDirLeft | kDirRight);
    if (dirs == 0U) {
      return (mask & kDirPoint) != 0U ? L'\u00b7' : L' ';
    }
    if (dirs == (kDirLeft | kDirRight)) {
      return L'\u2500';
    }
    if (dirs == (kDirUp | kDirDown)) {
      return L'\u2502';
    }
    if (dirs == (kDirDown | kDirRight)) {
      return L'\u250c';
    }
    if (dirs == (kDirDown | kDirLeft)) {
      return L'\u2510';
    }
    if (dirs == (kDirUp | kDirRight)) {
      return L'\u2514';
    }
    if (dirs == (kDirUp | kDirLeft)) {
      return L'\u2518';
    }
    if (dirs == (kDirUp | kDirDown | kDirRight)) {
      return L'\u251c';
    }
    if (dirs == (kDirUp | kDirDown | kDirLeft)) {
      return L'\u2524';
    }
    if (dirs == (kDirLeft | kDirRight | kDirDown)) {
      return L'\u252c';
    }
    if (dirs == (kDirLeft | kDirRight | kDirUp)) {
      return L'\u2534';
    }
    if (dirs == (kDirUp | kDirDown | kDirLeft | kDirRight)) {
      return L'\u253c';
    }
    if ((dirs & static_cast<uint16_t>(kDirLeft | kDirRight)) != 0U) {
      return L'\u2500';
    }
    return L'\u2502';
  };
  for (int cell_y = 0; cell_y < canvas.height; ++cell_y) {
    for (int cell_x = 0; cell_x < canvas.width; ++cell_x) {
      const uint16_t bits = canvas.cells[static_cast<size_t>(cell_y * canvas.width + cell_x)];
      if (bits == 0U) {
        continue;
      }
      const wchar_t glyph_char = glyphForMask(bits);
      const wchar_t glyph[2] = {glyph_char, L'\0'};
      mvwaddnwstr(win, top + cell_y, left + cell_x, glyph, 1);
    }
  }
  if (has_colors()) {
    wattroff(win, COLOR_PAIR(color_pair));
  }
}

int estimateCpuRows(const Snapshot& snapshot) {
  int rows = 0;
  rows += 5;  // CPU model, topology, temp, speed, usage text.
  if (snapshot.cpu.usage_percent) {
    ++rows;
  }
  if (snapshot.cpu.temperature_c) {
    ++rows;
  }
  return rows;
}

int estimateRamRows(const Snapshot& snapshot) {
  if (!snapshot.ram.total_kb || !snapshot.ram.available_kb || *snapshot.ram.total_kb <= 0) {
    return 1;  // N/A line.
  }
  return 3;  // Used, available, usage bar.
}

int estimateGpuRows(const Snapshot& snapshot) {
  int rows = 0;
  if (snapshot.gpus.empty()) {
    rows += 2;  // no-gpu message + tip
  } else {
    if (!anyGpuHasTelemetry(snapshot.gpus)) {
      rows += static_cast<int>(snapshot.gpus.size());  // Card 1, Card 2, ...
      return rows;
    }

    const size_t display_gpu_index = pickDisplayGpuIndex(snapshot.gpus);
    const GpuMetrics& gpu = snapshot.gpus[display_gpu_index];
    rows += 6;  // GPU base lines.
    if (!gpu.memory_used_mib) {
      ++rows;
    }
    if (gpu.utilization_percent) {
      ++rows;
    }
    if (gpu.memory_utilization_percent) {
      ++rows;
    }
    if (countAdditionalRelevantGpus(snapshot.gpus, display_gpu_index) > 1) {
      ++rows;
    }
  }
  return rows;
}

int estimateDiskRows(const Snapshot& snapshot) {
  int rows = 1;  // Mount.
  if (!snapshot.disk.total_bytes || !snapshot.disk.free_bytes || *snapshot.disk.total_bytes == 0) {
    ++rows;  // unavailable line.
  } else {
    rows += 2;  // free line + used bar.
  }
  return rows;
}

int estimateHistoryRows() {
  return 12;  // Legend + graph + listed metrics.
}

void splitColumnHeights(int total_h, int top_pref_h, int bottom_pref_h, int gap, int* top_h, int* bottom_h) {
  const int min_panel_h = 4;
  const int available = std::max(min_panel_h * 2, total_h - gap);
  const int pref_sum = std::max(1, top_pref_h + bottom_pref_h);

  int proposed_top =
      static_cast<int>(std::round(static_cast<double>(available) * static_cast<double>(top_pref_h) /
                                  static_cast<double>(pref_sum)));
  proposed_top = std::max(min_panel_h, std::min(available - min_panel_h, proposed_top));

  *top_h = proposed_top;
  *bottom_h = available - proposed_top;
}

void renderCpuPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) {
    return;
  }
  int row = 1;
  addWindowLine(panel, row++, "CPU: " + snapshot.cpu.name);
  addWindowLine(panel, row++, "Topology: " + formatCpuTopology(snapshot.cpu));
  addWindowLine(panel, row++, "Speed: " + formatCpuFrequency(snapshot.cpu.frequency_mhz));

  if (snapshot.cpu.usage_percent && row < getmaxy(panel) - 1) {
    drawPipeBar(panel, row++, "Usage", *snapshot.cpu.usage_percent);
  }
  if (snapshot.cpu.temperature_c && row < getmaxy(panel) - 1) {
    drawPipeBar(panel, row++, "Temp ", *snapshot.cpu.temperature_c , 1);
  }
}

void renderNetworkPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) {
    return;
  }
  int row = 1;
}

void renderRamPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) {
    return;
  }
  int row = 1;

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

  const double used_pct = (total_kb > 0) ? (100.0 * static_cast<double>(used_kb) / static_cast<double>(total_kb))
                                         : 0.0;

  addWindowLine(panel, row++, "Used: " + humanBytes(used_bytes) + " / " + humanBytes(total_bytes));
  addWindowLine(panel, row++, "Available: " + humanBytes(available_bytes));
  if (row < getmaxy(panel) - 1) {
    drawPipeBar(panel, row++, "Usage", used_pct);
  }
}

void renderGpuPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) {
    return;
  }
  int row = 1;

  if (!snapshot.gpus.empty()) {
    const size_t display_gpu_index = pickDisplayGpuIndex(snapshot.gpus);
    const GpuMetrics& gpu = snapshot.gpus[display_gpu_index];
    const auto in_use_gpu_index = pickInUseGpuIndex(snapshot.gpus);

    if (!anyGpuHasTelemetry(snapshot.gpus)) {
      size_t listed = 0;
      for (size_t i = 0; i < snapshot.gpus.size() && row < getmaxy(panel) - 1; ++i) {
        const GpuMetrics& item = snapshot.gpus[i];
        std::string line = item.name + " [" + item.source + "]";
        if (in_use_gpu_index && *in_use_gpu_index == i) {
          line += " (in use)";
        }
        addWindowLine(panel, row++, line);
        ++listed;
      }

      if (listed < snapshot.gpus.size() && row < getmaxy(panel) - 1) {
        const size_t remaining = snapshot.gpus.size() - listed;
        if (remaining > 1) {
          addWindowLine(panel, row++, "+" + std::to_string(remaining) + " more GPU(s)");
        }
      }
      return;
    }

    std::string gpu_header = "GPU: " + gpu.name + " [" + gpu.source + "]";
    if (in_use_gpu_index && *in_use_gpu_index == display_gpu_index) {
      gpu_header += " (in use)";
    }
    addWindowLine(panel, row++, gpu_header);

    addWindowLine(panel, row++, "Temperature: " + formatOptional(gpu.temperature_c, " C", 1));
    addWindowLine(panel, row++, "Speed: " + formatOptional(gpu.core_clock_mhz, " MHz", 0));
    addWindowLine(panel, row++, "Usage: " + formatOptional(gpu.utilization_percent, "%", 0));
    addWindowLine(panel, row++, "Power: " + formatOptional(gpu.power_w, " W", 1));
    addWindowLine(panel, row++, "VRAM: " + formatGpuVramUsage(gpu));

    if (!gpu.memory_used_mib && row < getmaxy(panel) - 1) {
      addWindowLine(panel, row++, "VRAM source not exposed");
    }

    if (gpu.utilization_percent && row < getmaxy(panel) - 1) {
      drawPipeBar(panel, row++, "Util", *gpu.utilization_percent);
    }
    if (gpu.memory_utilization_percent && row < getmaxy(panel) - 1) {
      drawPipeBar(panel, row++, "VRAM", *gpu.memory_utilization_percent);
    }

    const size_t extra_relevant_gpu_count = countAdditionalRelevantGpus(snapshot.gpus, display_gpu_index);
    if (extra_relevant_gpu_count > 1 && row < getmaxy(panel) - 1) {
      addWindowLine(panel, row++, "+" + std::to_string(extra_relevant_gpu_count) + " more GPU(s)");
    }
  } else {
    addWindowLine(panel, row++, "No GPU telemetry found");
    addWindowLine(panel, row++, "Tip: install NVIDIA drivers / sensors");
  }
}

void renderDiskPanel(WINDOW* panel, const Snapshot& snapshot) {
  if (!panel) {
    return;
  }
  int row = 1;
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
  if (row < getmaxy(panel) - 1) {
    drawPipeBar(panel, row++, "Used", used_pct);
  }
}

void renderHistoryPanel(WINDOW* panel, const MetricsHistory& history, const std::vector<ProcessInfo>& processes) {
  if (!panel) {
    return;
  }

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
  addLegendItem("VRAM", 5);
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
  BrailleCanvas vram_cells = createBrailleCanvas(graph_w, graph_h);
  BrailleCanvas disk_cells = createBrailleCanvas(graph_w, graph_h);

  plotBrailleSeries(&cpu_cells, history.cpu_usage, 0.0, 100.0);
  plotBrailleSeries(&cpu_temp_cells, history.cpu_temp, 0.0, 100.0);
  plotBrailleSeries(&ram_cells, history.ram_usage, 0.0, 100.0);
  plotBrailleSeries(&gpu_cells, history.gpu_usage, 0.0, 100.0);
  plotBrailleSeries(&vram_cells, history.gpu_vram_usage, 0.0, 100.0);
  plotBrailleSeries(&disk_cells, history.disk_usage, 0.0, 100.0);

  drawBrailleLayer(panel, disk_cells, graph_top, graph_left, 6);
  drawBrailleLayer(panel, ram_cells, graph_top, graph_left, 2);
  drawBrailleLayer(panel, cpu_temp_cells, graph_top, graph_left, 3);
  drawBrailleLayer(panel, gpu_cells, graph_top, graph_left, 1);
  drawBrailleLayer(panel, vram_cells, graph_top, graph_left, 5);
  drawBrailleLayer(panel, cpu_cells, graph_top, graph_left, 4);

  if (!has_table) {
    return;
  }

  const int separator_row = table_top - 1;
  for (int x = 1; x < max_x - 1; ++x) {
    mvwaddch(panel, separator_row, x, ACS_HLINE);
  }
  for (int x = 1; x < max_x - 1; ++x) {
    mvwaddch(panel, table_top -2, x, ACS_HLINE);
  }
  

  int table_row = table_top;
  addColoredText(panel, table_row++, 2, "PID    CPU%   MEM%   COMMAND", 7);
  const int max_entries = table_rows - 1;
  for (int i = 0; i < max_entries; ++i) {
    if (i >= static_cast<int>(processes.size())) {
      break;
    }
    const auto& process = processes[static_cast<size_t>(i)];
    std::ostringstream line;
    line << std::setw(6) << process.pid << " " << std::setw(6) << std::fixed << std::setprecision(1)
         << process.cpu_percent << " " << std::setw(6) << std::fixed << std::setprecision(1) << process.mem_percent
         << " " << process.command;
    addWindowLine(panel, table_row++, line.str());
  }
}

void renderSnapshot(const Snapshot& snapshot, const MetricsHistory& history,
                    const std::vector<ProcessInfo>& processes, const std::string& host) {
  erase();

  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);

  if (rows < 18 || cols < 80) {
    attron(A_BOLD);
    attroff(A_BOLD);
    mvaddnstr(2, 2, "Terminal too small. Resize to at least 80x18.", std::max(0, cols - 4));
    mvaddnstr(3, 2, "Press q to quit.", std::max(0, cols - 4));
    refresh();
    return;
  }

  attron(A_BOLD);
  attroff(A_BOLD);
  std::string header = "Host: " + host + "   Time: " + currentTimestamp();
  mvaddnstr(1, 2, header.c_str(), cols - 4);
  mvaddnstr(rows - 1, 2, "Press q to quit", cols - 4);

  const int top = 3;
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
  const int min_stack_h = min_panel_h * 2 + gap;
  const int left_pref_top_h = estimateCpuRows(snapshot) + 2;
  const int left_pref_bottom_h = estimateRamRows(snapshot) + 2;
  const int right_pref_top_h = estimateGpuRows(snapshot) + 2;
  const int right_pref_bottom_h = estimateDiskRows(snapshot) + 2;
  const int pref_stack_h = std::max(left_pref_top_h + gap + left_pref_bottom_h,
                                    right_pref_top_h + gap + right_pref_bottom_h);
  const int stack_h = std::min(content_h, std::max(min_stack_h, pref_stack_h));
  const int remaining_h = content_h - stack_h;
  const bool has_history_panel = remaining_h >= (history_min_h + gap);
  const int history_h = has_history_panel ? (remaining_h - gap) : 0;

  int cpu_h = min_panel_h;
  int ram_h = min_panel_h;
  int gpu_h = min_panel_h;
  int disk_h = min_panel_h;
  splitColumnHeights(stack_h, left_pref_top_h, left_pref_bottom_h, gap, &cpu_h, &ram_h);
  splitColumnHeights(stack_h, right_pref_top_h, right_pref_bottom_h, gap, &gpu_h, &disk_h);

  const Rect cpu_rect{top, x_left, cpu_h, left_w};
  const Rect ram_rect{top + cpu_h + gap, x_left, ram_h, left_w};
  const Rect gpu_rect{top, x_right, gpu_h, right_w};
  const Rect disk_rect{top + gpu_h + gap, x_right, disk_h, right_w};
  const Rect history_rect{top + stack_h + gap, margin, history_h, cols - 2 * margin};
  const Rect net_rect {top + ram_h + gap , x_left , ram_h , left_w};
  WINDOW* cpu_panel = createPanel(cpu_rect, "CPU");
  WINDOW* ram_panel = createPanel(ram_rect, "RAM");
  WINDOW* gpu_panel = createPanel(gpu_rect, "GPU");
  WINDOW* net_panel = createPanel(net_rect , "NET");
  WINDOW* disk_panel = createPanel(disk_rect, "Disk");
  WINDOW* history_panel = has_history_panel ? createPanel(history_rect, "Activity") : nullptr;

  renderCpuPanel(cpu_panel, snapshot);
  renderRamPanel(ram_panel, snapshot);
  renderGpuPanel(gpu_panel, snapshot);
  renderDiskPanel(disk_panel, snapshot);
  renderHistoryPanel(history_panel, history, processes);

  wnoutrefresh(stdscr);

  if (cpu_panel) {
    wnoutrefresh(cpu_panel);
    delwin(cpu_panel);
  }
  if (ram_panel) {
    wnoutrefresh(ram_panel);
    delwin(ram_panel);
  }
  if (gpu_panel) {
    wnoutrefresh(gpu_panel);
    delwin(gpu_panel);
  }
  if (disk_panel) {
    wnoutrefresh(disk_panel);
    delwin(disk_panel);
  }
  if (history_panel) {
    wnoutrefresh(history_panel);
    delwin(history_panel);
  }

  doupdate();
}

Snapshot collectSnapshot() {
  Snapshot snapshot;
  snapshot.cpu = collectCpuMetrics();
  snapshot.ram = collectRam();
  snapshot.disk = collectDisk("/");
  snapshot.gpus = collectGpus();
  return snapshot;
}

int main() {
  std::setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  timeout(kFrameIntervalMs);

  if (has_colors()) {
    start_color();
    use_default_colors();
    if (can_change_color() && COLORS >= 32) {
      constexpr short kSoftGreen = 20;
      constexpr short kSoftAmber = 21;
      constexpr short kSoftRose = 22;
      constexpr short kSoftCyan = 23;
      constexpr short kSoftLavender = 24;
      constexpr short kSoftBlue = 25;
      constexpr short kSoftGray = 26;

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
      init_pair(1, COLOR_CYAN, -1);
      init_pair(2, COLOR_BLUE, -1);
      init_pair(3, COLOR_MAGENTA, -1);
      init_pair(4, COLOR_CYAN, -1);
      init_pair(5, COLOR_MAGENTA, -1);
      init_pair(6, COLOR_BLUE, -1);
      init_pair(7, COLOR_WHITE, -1);
    }
  }

  const std::string host = hostName();
  MetricsHistory history;
  constexpr size_t kHistoryPoints = 2048;
  constexpr size_t kProcessRows = 6;
  Snapshot snapshot = collectSnapshot();
  std::vector<ProcessInfo> processes = collectTopProcesses(kProcessRows);
  updateHistory(&history, snapshot, kHistoryPoints, computeRootDiskBusyPercent());
  renderSnapshot(snapshot, history, processes, host);

  while (true) {
    const int ch = getch();
    if (ch == 'q' || ch == 'Q') {
      break;
    }

    if (ch == KEY_RESIZE) {
      renderSnapshot(snapshot, history, processes, host);
      continue;
    }

    if (ch != ERR) {
      continue;
    }

    snapshot = collectSnapshot();
    processes = collectTopProcesses(kProcessRows);
    updateHistory(&history, snapshot, kHistoryPoints, computeRootDiskBusyPercent());
    renderSnapshot(snapshot, history, processes, host);
  }

  endwin();
  return 0;
}
