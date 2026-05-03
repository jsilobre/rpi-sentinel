# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**rpi-sentinel** is a C++23 real-time environmental sensor monitoring daemon for Raspberry Pi. It polls hardware sensors (DS18B20 temperature, DHT11 temperature/humidity) or simulated sensors in parallel threads, evaluates thresholds with hysteresis, and dispatches events to a handler chain that logs, stores to SQLite, publishes via MQTT, and updates a local web dashboard.

## Documentation

Detailed technical references live in `docs/`:

| File | Contents |
|---|---|
| `docs/architecture.md` | Layer diagram, component table, class diagram, C++23 features used |
| `docs/workflow.md` | Per-cycle polling loop, threading model, hysteresis numerical example, sensor factory |
| `docs/build-guide.md` | CMake dependency graph, cross-compilation, RPi hardware setup, adding a test |
| `docs/persistence.md` | SQLite schema & PRAGMAs, rotation policy, MQTT history-on-demand protocol, failure modes |

## Build Commands

Requires GCC 14+, CMake 3.28+, `libsqlite3-dev`. MQTT support is auto-detected if `libmosquitto-dev` is installed.

```bash
# Install prerequisites (Ubuntu 24.04)
sudo apt-get install -y g++-14 cmake ninja-build libsqlite3-dev libmosquitto-dev

# Configure (with tests)
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Build
cmake --build build --parallel

# Run all tests
ctest --test-dir build --output-on-failure --parallel 4

# Run a single test by filter (all tests share one binary: build/rpi_tests)
./build/rpi_tests --gtest_filter="EventBus*"
./build/rpi_tests --gtest_filter="ThresholdMonitor*"

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

### Adding a Test

Add a `.cpp` file in `tests/` using Google Test, then register it in `tests/CMakeLists.txt` by appending it to the `add_executable(rpi_tests ...)` list. Google Test is fetched automatically via `FetchContent` at CMake configure time.

## Architecture

### Layered Design

Five layers communicate only through abstract interfaces. The dependency rule is strict — no lower layer depends on a higher one:

```
main → monitoring → sensors
                  → events → alerts → persistence
                  → web → persistence (read-only at startup)
```

Each layer is compiled as a static library linked into the final `rpi-sentinel` executable. See `docs/architecture.md` for the full class diagram.

### Data Flow Per Polling Cycle

Each `ThresholdMonitor` runs in its own `std::jthread`:
1. `sensor.read()` → `std::expected<SensorReading, SensorError>`
2. `EventBus::dispatch(ReadingEvent)` → all handlers in the main thread of dispatch
3. Evaluate warn/crit thresholds with hysteresis → dispatch `ThresholdExceeded` or `ThresholdRecovered` if state changed
4. `std::this_thread::sleep_for(poll_interval)`

`EventBus` dispatch is **synchronous** — `on_event()` is called directly in the monitoring thread. Handlers must therefore be non-blocking. `EventBus` uses a single `std::mutex` to serialize concurrent dispatches from multiple monitor threads.

### Startup Sequence (`src/main.cpp`)

1. Parse CLI args, load `config.json` via `ConfigLoader`
2. Construct `EventBus` and register all enabled handlers
3. If persistence enabled: open `HistoryStore`, prime `WebState` with last 120 readings per sensor from SQLite
4. Construct `MonitoringHub` → creates sensors + monitors from config
5. `hub.start()` → spawns jthreads
6. Start `HttpServer` if `web_enabled`
7. Block on SIGINT/SIGTERM, then `hub.stop()` + cleanup

### Web API Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Dashboard HTML (Chart.js, auto-refresh every 2 s) |
| `/api/state` | GET | Current readings, 120-point history, and recent alerts (JSON) |
| `/api/config` | GET | Sensor thresholds |
| `/api/config` | POST | Update thresholds at runtime without restart (auth) |
| `/api/refresh` | POST | Force an immediate sensor poll (auth) |

### Web auth (optional)

Set `web_auth.enabled: true` in `config.json` to require a bearer token on
mutating POST routes. The active token is resolved at startup from the env
var named in `web_auth.token_env` (defaults to `RPI_SENTINEL_WEB_TOKEN`);
a literal `web_auth.token` is a dev-only fallback. When auth is enabled but
no token can be resolved, the daemon refuses to start the HTTP server. GET
routes and the SSE stream remain unauthenticated.

Clients send `Authorization: Bearer <token>` on POSTs. The bundled dashboard
prompts the user on the first 401 and persists the token in `localStorage`.

### MQTT History-on-Demand Protocol

The cloud `dashboard/index.html` (hosted on GitHub Pages) requests history per sensor over MQTT on page load — no separate backend needed.

- Dashboard publishes to `rpi/history/req`: `{"request_id": "<uuid>", "sensor_id": "<id>", "limit": 120}`
- `MqttPublisher` queries `HistoryStore` and responds on `rpi/history/resp/{request_id}`
- Points are returned in ascending chronological order; `truncated: true` signals the 500-point server-side cap was hit

See `docs/persistence.md` for the full payload schema, hydration sequence, and HiveMQ ACL requirements.

## Key Conventions

### Error Handling

Hardware boundaries use `std::expected<T, SensorError>` — never throw exceptions in sensor readers. Callers check `.has_value()` before using the result. Transient read errors are logged and the cycle is skipped (no `Reading` event dispatched).

### C++23 Features in Use

`std::jthread` (auto stop_token for cooperative shutdown), `std::expected`, `std::println`, `std::format`, `std::numbers::pi_v`, designated initializers. The project requires C++23 — use these features where appropriate rather than C++17 equivalents.

### Zero-Warning Policy

The project builds with `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast`. CI fails on any warning. Fix all warnings before committing.

### Adding a New Sensor Type

1. Create a class in `src/sensors/` implementing `ISensorReader` (`read()` + `sensor_id()`)
2. Add the type to `SensorType` enum in `Config.hpp`
3. Instantiate it in the `make_sensor()` factory in `MonitoringHub.cpp`
4. No changes needed in `EventBus`, alerts, or persistence layers

### Adding a New Alert Handler

1. Implement `IAlertHandler` in `src/alerts/` (single method: `on_event(const SensorEvent&)`)
2. Register the handler in `main.cpp` (conditioned on config flag)
3. Keep `on_event()` non-blocking — it runs synchronously in the monitoring thread
4. No changes needed in `MonitoringHub` or sensors

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
    { "id": "temp1", "type": "simulated", "metric": "temperature", "threshold_warn": 30.0, "threshold_crit": 40.0 }
  ]
}
```

Sensor `type` values: `simulated`, `ds18b20`, `dht11`. DS18B20 requires `device_path` pointing to `/sys/bus/w1/devices/<id>/temperature`. The `data/` directory is created automatically at runtime.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and PR: installs GCC 14 + Ninja, configures with `BUILD_TESTING=ON`, builds, then runs `ctest --output-on-failure --parallel 4`.

`deploy-dashboard.yml` deploys `dashboard/` to GitHub Pages on pushes to `main` that touch `dashboard/**`, injecting MQTT credentials from GitHub Secrets by replacing `__MQTT_BROKER_WSS__`, `__MQTT_USER__`, `__MQTT_PASS__` placeholders in `dashboard/index.html`.
