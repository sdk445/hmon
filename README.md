<p align="center">
  <img width="1095" height="577" alt="hmon" src="https://github.com/user-attachments/assets/95d62f59-1005-4cbd-86e8-d0b0494a341c" />
</p>

<h1 align="center">hmon</h1>
<p align="center">A fast, lightweight Linux system monitor built in C++ and ncurses.</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#modes">Modes</a> •
  <a href="#installation">Installation</a> •
  <a href="#usage">Usage</a> •
  <a href="#controls">Controls</a> •
  <a href="#telemetry-sources">Telemetry Sources</a>
</p>

---

## Features

- **CPU** — usage, temperature, frequency, per-core cycle breakdown
- **GPU** — utilization, temperature, clock, power draw, VRAM (NVIDIA + AMD/Intel fallback)
- **Memory** — usage, buffers, cache, slab, swap, detailed `/proc/meminfo` breakdown
- **Disk** — space usage, per-device I/O throughput and busy %
- **Network** — interface throughput (RX/TX), TCP connection states
- **Processes** — top processes sorted by CPU, memory, GPU, or PID
- **Docker** — container status, CPU/memory per container
- **Ports** — listening ports with bound process
- **Services** — systemd unit status
- **History** — activity graphs with braille-character rendering

## Modes

### Default

Full dashboard with paneled layout — CPU, RAM, GPU, disk, network, and activity history.

<img width="1920" height="1080" alt="default mode" src="https://github.com/user-attachments/assets/66df0127-5ca5-40cd-93cd-bbe13564e775" />

### Zen Mode

Consolidated single-screen view with Docker, ports, services, databases, cron, and top processes. Toggle with `z`.

<img width="1920" height="1080" alt="zen mode" src="https://github.com/user-attachments/assets/2bee6e79-d8a1-46b6-9410-ca6759416669" />

### Pro Mode

<!-- TODO: Add pro mode screenshot -->
<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/c4abbf21-cfc0-49c3-bac0-3f21450b322e" />

Dense two-column layout with kernel stats, per-core CPU cycles, detailed memory breakdown, file descriptors, GPU, network connections, disk I/O.Toggle with `P`.

---

## Installation

### Requirements

| Dependency | Version |
|---|---|
| Linux | any |
| C++ compiler | C++20 (`g++` or `clang++`) |
| CMake | 3.16+ |
| ncurses | dev package |

**Ubuntu/Debian:**

```bash
sudo apt install -y build-essential cmake libncurses-dev
```

**Optional** — NVIDIA GPU telemetry:

```bash
sudo apt install -y nvidia-utils-<version>
```

### Build

```bash
cmake -S . -B build
cmake --build build -j
```

### Install

```bash
sudo cmake --install build
```

Or with a custom prefix:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
sudo cmake --install build
```

---

## Usage

```bash
hmon [OPTIONS]
```

| Option | Description |
|---|---|
| `-h`, `--help` | Show help and exit |
| `-v`, `--version` | Show version and exit |
| `-r`, `--refresh <secs>` | Refresh interval in seconds (1–60, default: 1) |
| `-t`, `--top <count>` | Number of top processes to show (1–20, default: 8) |
| `--no-gpu` | Disable GPU telemetry |
| `--no-history` | Disable activity history graphs |
| `--zen` | Start in zen mode |
| `--pid <id>` | Lock focus on a specific PID |
| `--no-color` | Disable colors |

---

## Controls

| Key | Action |
|---|---|
| `q` | Quit |
| `?` | Toggle help overlay |
| `z` | Toggle zen mode |
| `P` | Toggle pro mode |
| `s` | Cycle sort (CPU → MEM → GPU → PID) |
| `j` / `k` / `↑` / `↓` | Navigate process list |
| `1`–`9` | Jump to process row |
| `l` | Lock/unlock selected PID |
| `u` | Unlock PID |
| `r` | Force refresh |
| `+` / `-` | Increase/decrease refresh speed |
| `Tab` | Cycle focus (zen mode) |

---

## Telemetry Sources

| Metric | Source |
|---|---|
| CPU usage | `/proc/stat` delta sampling |
| CPU temperature | `/sys/class/thermal/*` |
| CPU frequency | `/sys/devices/system/cpu/*/cpufreq`, `/proc/cpuinfo` |
| Memory | `/proc/meminfo` |
| Network | `/proc/net/dev` |
| Disk space | `statvfs("/")` |
| Disk I/O | `/sys/block/*/stat` |
| GPU (NVIDIA) | `nvidia-smi` |
| GPU (AMD/Intel) | `/sys/class/drm/*/device` + hwmon |
| Docker | Docker Engine API (`/var/run/docker.sock`) |
| Ports | `/proc/net/tcp`, `/proc/net/tcp6` |
| Services | `systemctl` |

---

## Lint

```bash
cmake -S . -B build
cmake --build build --target lint
```

Enable lint checks on every build:

```bash
cmake -S . -B build -DHMON_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

---
<p align="center">
  <a href="https://github.com/sponsors/sdk445">
    <img src="https://img.shields.io/badge/Sponsor-❤️-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors" />
  </a>
  &nbsp;&nbsp;
  <a href="https://buymeacoffee.com/sdk445">
    <img src="https://img.buymeacoffee.com/button-api/?text=Buy%20me%20a%20coffee&emoji=&slug=sdk445&button_colour=FFDD00&font_colour=000000&font_family=Lato&outline_colour=000000&coffee_colour=ffffff" alt="Buy Me a Coffee" />
  </a>
</p>
<p align="center">MIT License © 2026 Chinmoy</p>
