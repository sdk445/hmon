# hmon CLI

`hmon` is a Linux resource monitor.
<img width="1898" height="977" alt="image" src="https://github.com/user-attachments/assets/95d98d9d-947c-4308-b7ed-67e3bec0dc9c" />

- Disk space left
- CPU temperature, speed, usage.
- GPU temperature, speed, usage, wattage, and VRAM usage
- RAM consumption
- DISK consumption
- TOP processes

## Requirements

- Linux
- C++17 compiler (`g++` or `clang++`)
- CMake 3.16+
- `ncurses` development package

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake libncurses-dev
```

Optional (for NVIDIA GPU telemetry):

```bash
sudo apt install -y nvidia-utils-<version>
```

Optional (for linting):

```bash
sudo apt install -y clang-tidy
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Lint

Run lint checks globally:

```bash
cmake -S . -B build
cmake --build build --target lint
```

Enable lint checks globally during every build:

```bash
cmake -S . -B build -DHMON_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

## Run

```bash
./build/hmon
```

## Notes on telemetry sources

- CPU temp: `/sys/class/thermal/*`
- CPU speed: `/sys/devices/system/cpu/*/cpufreq` or `/proc/cpuinfo`
- CPU usage: `/proc/stat` delta sampling
- RAM: `/proc/meminfo`
- Disk: `statvfs("/")`
- GPU:
  - Primary: `nvidia-smi` (temp, core clock, fan, utilization, power draw, memory used/total)
  - Fallback: `/sys/class/drm/*/device` + hwmon + `sensors` command
