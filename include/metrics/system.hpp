#pragma once

#include <string>

#include "metrics/types.hpp"

RamMetrics collectRam();
DiskMetrics collectDisk(const std::string& mount_point = "/");

std::string currentTimestamp();
std::string hostName();
std::string humanBytes(unsigned long long bytes);
