#pragma once

#include <optional>

struct SensorsFallbackMetrics {
  std::optional<double> cpu_fan_rpm;
  std::optional<double> cpu_power_w;
  std::optional<double> gpu_fan_rpm;
  std::optional<double> gpu_power_w;
};

SensorsFallbackMetrics collectSensorsFallbackMetrics();
