<p align="center">
  <img src="https://github.com/user-attachments/assets/6e2a08b7-3c65-4700-a9db-e0d3580f8d41">
</p>
<h1 align="center">hmon is a Linux resource monitor.</h1>


<p align="center">
  <video src="https://github.com/user-attachments/assets/1dc15b42-e694-4bcc-bd98-9ef2fd17d9d1"
         controls
         width="800">
  </video>
</p>


<!-- <img width="1907" height="966" alt="image" src="https://github.com/user-attachments/assets/28522c36-2218-41ce-b03a-a46733043969" /> -->
<img width="1919" height="1063" alt="image" src="https://github.com/user-attachments/assets/2882f417-b740-4905-8f6f-80dc18bb707f" />


- Disk space left
- CPU temperature, speed, usage.
- GPU temperature, speed, usage, wattage, and VRAM usage
- RAM consumption
- Network throughput  
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

## Install

```bash
cmake --build build --target install
```

Or with a custom prefix:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --target install
```

## Notes on telemetry sources

- CPU temp: `/sys/class/thermal/*`
- CPU speed: `/sys/devices/system/cpu/*/cpufreq` or `/proc/cpuinfo`
- CPU usage: `/proc/stat` delta sampling
- RAM: `/proc/meminfo`
- Network: `/proc/net/dev` 
- Disk: `statvfs("/")`
- GPU:
  - Primary: `nvidia-smi` (temp, core clock, fan, utilization, power draw, memory used/total)
  - Fallback: `/sys/class/drm/*/device` + hwmon + `sensors` command
