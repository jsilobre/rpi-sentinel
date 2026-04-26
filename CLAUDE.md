# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**rpi-sentinel** is a C++23 real-time environmental sensor monitoring daemon for Raspberry Pi. It polls hardware sensors (DS18B20 temperature, DHT11 temperature/humidity) or simulated sensors in parallel threads, evaluates thresholds with hysteresis, and dispatches events to a handler chain that logs, stores to SQLite, publishes via MQTT, and updates a local web dashboard.

## Build Commands

Requires GCC 14+, CMake 3.28+, `libsqlite3-dev`. MQTT support is auto-detected if `libmosquitto-dev` is installed.

```bash
# Install prerequisites (Ubuntu 24.04)
sudo apt-get install -y g++-14 cmake ninja-build libsqlite3-dev libmosquitto-dev

# Configure
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release

# Configure with tests
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Build
cmake --build build --parallel

# Run all tests
ctest --test-dir build --output-on-failure --parallel 4

# Run a single test binary
./build/tests/test_event_bus
./build/tests/test_threshold_monitor

# Run the daemon (requires config.json in working dir, or pass path as arg)
./build/rpi-sentinel [path/to/config.json]

# Set up LSP (clangd)
ln -s build/compile_commands.json compile_commands.json
```

Cross-compiling for ARM64:
```bash
cmake -B build-arm \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14 \
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm --parallel
```

## Architecture

### Layered Design

Five layers communicate only through abstract interfaces — upper layers depend on lower, never vice versa:

```
sensors       ISensorReader (pure virtual) ← DS18B20Reader | DHT11Reader | SimulatedSensor
   ↓
monitoring    ThresholdMonitor polls sensors; MonitoringHub owns N monitors in jthreads
   ↓
events        EventBus: mutex-protected synchronous dispatch to registered IAlertHandlers
   ↓
alerts        LogAlert | WebAlert | SqliteHistoryHandler | MqttPublisher
   ↓
persistence   HistoryStore (SQLite WAL) — also read at startup to prime WebState
web           HttpServer + WebState — REST API + static dashboard assets
```

### Data Flow Per Polling Cycle

Each `ThresholdMonitor` runs in its own `std::jthread`:
1. `sensor.read()` → `std::expected<SensorReading, SensorError>`
2. `EventBus::dispatch(ReadingEvent)` → all handlers receive every reading
3. Evaluate warn/crit thresholds with hysteresis → dispatch `ThresholdExceeded` or `ThresholdRecovered` if state changed
4. `std::this_thread::sleep_for(poll_interval)`

`EventBus` uses a single `std::mutex` to serialize dispatches from concurrent monitor threads.

### Startup Sequence (`src/main.cpp`)

1. Parse CLI args, load `config.json` via `ConfigLoader`
2. Construct `EventBus` and register all enabled handlers
3. If persistence enabled: open `HistoryStore`, prime `WebState` with last 120 readings per sensor from SQLite
4. Construct `MonitoringHub` → creates sensors + monitors from config
5. `hub.start()` → spawns jthreads
6. Start `HttpServer` if `web_enabled`
7. Block on SIGINT/SIGTERM, then `hub.stop()` + cleanup

### MQTT History-on-Demand Protocol

The cloud `dashboard/index.html` requests history over MQTT when it connects:
- Publishes to `rpi/history/request` with JSON `{"sensor": "<id>", "limit": N}`
- `MqttHistoryResponder` queries SQLite and publishes response to `rpi/history/response`
- This allows the cloud dashboard to hydrate charts without a server-side API

## Key Conventions

### Error Handling

Hardware boundaries use `std::expected<T, SensorError>` — never throw exceptions in sensor readers. Callers check `.has_value()` before using the result. This is the required pattern for all new sensor implementations.

### C++23 Features in Use

`std::jthread` (auto stop_token for cooperative shutdown), `std::expected`, `std::println`, `std::format`, `std::numbers::pi_v`, designated initializers. The project requires C++23 — use these features where appropriate.

### Zero-Warning Policy

The project builds with `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast`. CI fails on any warning. Fix all warnings before committing.

### Adding a New Sensor Type

1. Add a class in `src/sensors/` that implements `ISensorReader`
2. Register the new type string in `MonitoringHub` sensor factory
3. Add the type to `config.example.json`
4. No changes needed in `EventBus`, alerts, or persistence layers

### Adding a New Alert Handler

1. Implement `IAlertHandler` in `src/alerts/`
2. Register the handler in `main.cpp` (conditioned on config)
3. No changes needed in `MonitoringHub` or `EventBus`

## Configuration

Copy `config.example.json` to `config.json`. Key fields:

```json
{
  "hysteresis": 2.0,
  "poll_interval_ms": 5000,
  "web_enabled": true,
  "web_port": 8080,
  "mqtt": { "enabled": false, "broker_url": "", "username": "", "password": "", "topic_prefix": "rpi" },
  "history": { "enabled": true, "db_path": "data/history.db", "retention_days": 7, "max_points_per_sensor": 50000 },
  "sensors": [
    { "id": "temp1", "type": "simulated", "metric": "temperature_c", "threshold_warn": 30.0, "threshold_crit": 40.0 }
  ]
}
```

Sensor `type` values: `simulated`, `ds18b20`, `dht11`. DS18B20 requires `device_path` (e.g. `/sys/bus/w1/devices/28-xxxx/temperature`).

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and PR:
- Installs GCC 14, CMake, Ninja, libsqlite3-dev, libmosquitto-dev
- Configures with `BUILD_TESTING=ON` and builds with Ninja
- Runs `ctest --output-on-failure --parallel 4`

`deploy-dashboard.yml` deploys `dashboard/` to GitHub Pages on pushes to `main` that change dashboard files, injecting MQTT credentials from GitHub Secrets (`__MQTT_BROKER_WSS__`, `__MQTT_USER__`, `__MQTT_PASS__` placeholders in `dashboard/index.html`).
