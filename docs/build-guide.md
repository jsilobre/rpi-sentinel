# Build guide — rpi-sentinel

## Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| CMake | 3.28 | `cmake --version` |
| GCC | 14 | `g++-14 --version` — required for `<print>` (C++23) |
| Git | any | |
| ninja-build | any | optional but recommended |

**Ubuntu 24.04:**
```bash
sudo apt-get install -y g++-14 cmake ninja-build libsqlite3-dev libmosquitto-dev
```

`libsqlite3-dev` is required for the persistence layer (`HistoryStore`).
`libmosquitto-dev` is optional — it auto-enables the MQTT integration.

The daemon creates `data/` automatically next to the working directory (default
DB path `data/history.db`), so make sure the working directory is writable.

---

## Build system structure

```
CMakeLists.txt                 ← root: project, C++23 standard, options
cmake/
  CompilerOptions.cmake        ← apply_compiler_options(target) function
                                  flags: -Wall -Wextra -Wpedantic -Werror
src/
  CMakeLists.txt               ← subdirectories + rpi-sentinel executable
  alerts/CMakeLists.txt        ← static lib: alerts
  events/CMakeLists.txt        ← static lib: events  (depends on alerts)
  sensors/CMakeLists.txt       ← static lib: sensors
  monitoring/CMakeLists.txt    ← static lib: monitoring (depends on sensors, events)
  persistence/CMakeLists.txt   ← static lib: persistence (sqlite3, depends on alerts, events)
  web/CMakeLists.txt           ← static lib: web
tests/
  CMakeLists.txt               ← FetchContent GTest + rpi_tests executable
```

**CMake dependency graph:**

```
rpi-sentinel
  ├─ monitoring
  │    ├─ sensors
  │    └─ events
  │         └─ alerts (links persistence when ENABLE_MQTT=ON, for history responder)
  ├─ web
  └─ persistence (links sqlite3)
```

---

## Build commands

### Configure

```bash
# With tests (default)
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Without tests
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF

# With Ninja (faster)
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
```

### Compile

```bash
cmake --build build --parallel
# or with Ninja:
ninja -C build
```

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

### Run the application

```bash
./build/rpi-sentinel
```

Expected output (simulated sensors — BME280, DHT11, HC-SR501):
```
[main] Config loaded from 'config.json'
[MonitoringHub] 6 monitor(s) configured.
[main] Monitors started. Press Ctrl+C to stop.
[2026-04-23 14:13:11] [EXCEEDED]  sensor=bme280-pressure pressure=1016.4 threshold=1015.0
[2026-04-23 14:13:11] [EXCEEDED]  sensor=hcsr501-motion motion=1.0 threshold=0.9
[2026-04-23 14:13:36] [RECOVERED] sensor=hcsr501-motion motion=0.0 threshold=0.9
```

---

## CMake options

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTING` | `ON` | Enables building and registering Google Test unit tests |
| `CMAKE_BUILD_TYPE` | — | `Release` / `Debug` / `RelWithDebInfo` |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | `ON` | Generates `build/compile_commands.json` for clangd / VSCode |

---

## LSP support (clangd / VSCode)

`build/compile_commands.json` is generated automatically.

```bash
# Symlink so clangd finds it at the project root
ln -s build/compile_commands.json compile_commands.json
```

---

## CI/CD — GitHub Actions

The `.github/workflows/ci.yml` workflow triggers on every push and pull request.

### Steps

```
1. Code checkout
2. Install GCC 14 + CMake + Ninja
3. cmake configure  (-DBUILD_TESTING=ON)
4. cmake build      (--parallel)
5. ctest            (--output-on-failure)
```

### Merge requirements

- Build must pass without compilation errors (`-Werror` flags active).
- All unit tests must pass.

---

## Deploying to Raspberry Pi 5

### Option A — Native build on the RPi

Install tools directly on the RPi (Raspberry Pi OS 64-bit, Debian Bookworm based):

```bash
sudo apt-get install -y g++-14 cmake ninja-build git
git clone <repo>
cd rpi-sentinel
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Option B — Cross-compilation (PC → ARM64)

```bash
sudo apt-get install -y gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu

cmake -B build-arm \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14 \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-arm --parallel
# Copy build-arm/rpi-sentinel to the RPi via scp
```

### Enabling the 1-Wire bus on RPi 5

Add to `/boot/firmware/config.txt`:

```
dtoverlay=w1-gpio,gpiopin=4
```

Reboot, then verify:

```bash
ls /sys/bus/w1/devices/
# 28-xxxxxxxxxx   ← DS18B20 device id

cat /sys/bus/w1/devices/28-xxxxxxxxxx/temperature
# 23562  ← millidegrees Celsius (= 23.562 °C)
```

Set `device_path` in `config.json`:

```json
{
  "sensors": [{
    "id": "cpu-temp",
    "type": "ds18b20",
    "device_path": "/sys/bus/w1/devices/28-xxxxxxxxxx/temperature",
    "metric": "temperature",
    "threshold_warn": 60.0,
    "threshold_crit": 80.0
  }]
}
```

---

## Reading the CPU (SoC) temperature

No extra hardware needed — the kernel exposes the temperature via sysfs.

### Verify the thermal zone is available

```bash
cat /sys/class/thermal/thermal_zone0/temp
# 52000  ← millidegrees Celsius (= 52.0 °C)
```

If multiple zones exist (`thermal_zone1`, `thermal_zone2`, …) you can target a specific one
by overriding `device_path` in the sensor config:

```json
{
  "id": "cpu-temp",
  "type": "cpu_temp",
  "threshold_warn": 70.0,
  "threshold_crit": 85.0
}
```

When `device_path` is omitted the reader defaults to `thermal_zone0`.

---

## Adding a test

Tests live in `tests/`. They use Google Test (downloaded via `FetchContent` at `cmake configure` time).

```cpp
// tests/test_my_component.cpp
#include <gtest/gtest.h>
#include "../src/my_component/MyComponent.hpp"

TEST(MyComponent, DescribeTheCase)
{
    // Arrange
    // Act
    // Assert
    EXPECT_EQ(..., ...);
}
```

Register in `tests/CMakeLists.txt`:

```cmake
add_executable(rpi_tests
    test_event_bus.cpp
    test_threshold_monitor.cpp
    test_my_component.cpp   # ← add here
)
```
