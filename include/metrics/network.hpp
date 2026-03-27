#pragma once

#include <optional>
#include <string>

struct NetworkMetrics {
  std::string interface;
  std::optional<double> rx_kbps;
  std::optional<double> tx_kbps;
};

NetworkMetrics collectNetwork();
