# Architecture — rpi-sentinel

## 1. Design principles

- **Separation of concerns**: each layer only knows its immediate neighbours through abstract interfaces.
- **Testability**: the real sensor can be replaced by a simulator without modifying business logic.
- **Extensibility**: adding a sensor or an alert type only requires a new class implementing an existing interface.
- **Exception-free error handling**: `std::expected<T, E>` is used at the hardware boundary.
- **Config-driven parallelism**: any number of sensors of any metric type run in parallel, declared in `config.json`.

---

## 2. Layer view

```
┌─────────────────────────────────────────────────────────┐
│                        main.cpp                         │
│  (loads config, opens HistoryStore + primes WebState,   │
│   creates EventBus + handlers + MonitoringHub +         │
│   HttpServer, waits for shutdown signal)                │
└────────────┬───────────────────────────────┬────────────┘
             │                               │
             ▼                               ▼
┌────────────────────────┐       ┌──────────────────────────┐
│   MONITORING           │       │   ALERTS                 │
│  MonitoringHub         │──────►│  IAlertHandler           │
│  ThresholdMonitor [×N] │  via  │  LogAlert / WebAlert     │
│  Config / MonitorConfig│  bus  │  MqttPublisher           │
└────────┬───────────────┘       │  GpioAlert               │
         │                       │  SqliteHistoryHandler    │
         │                       └──────────┬───────────────┘
         │ read()                           │
         ▼                                 ▼
┌────────────────────┐           ┌───────────────────────┐
│   SENSORS          │           │   WEB                 │
│  ISensorReader     │           │  WebState             │
│  DS18B20Reader     │           │  HttpServer           │
│  DHT11Reader       │           └───────────────────────┘
│  CpuTempReader     │
│  SimulatedSensor   │
└────────────────────┘
                                 ┌───────────────────────┐
                                 │   EVENTS              │
                                 │  SensorEvent          │
                                 │  EventBus             │
                                 └───────────────────────┘
                                 ┌───────────────────────┐
                                 │   PERSISTENCE         │
                                 │  HistoryStore (SQLite)│
                                 │  SqliteHistoryHandler │
                                 └───────────────────────┘
```

### Dependency rule

```
main → monitoring → sensors
                  → events → alerts → persistence
                  → web → persistence (read-only at startup)
```

No lower layer depends on a higher layer.

---

## 3. Components

### 3.1 `sensors` layer

| File | Role |
|---|---|
| `ISensorReader.hpp` | Abstract interface. Defines `read()` and `sensor_id()`. |
| `DS18B20Reader.hpp/cpp` | Reads `/sys/bus/w1/devices/<id>/temperature` (Linux 1-Wire kernel driver). |
| `DHT11Reader.hpp/cpp` | Reads temperature and humidity via the Linux IIO kernel driver. |
| `CpuTempReader.hpp/cpp` | Reads the Raspberry Pi SoC temperature from `/sys/class/thermal/thermal_zone0/temp` (millidegrees Celsius, divided by 1000). The `device_path` field in `SensorConfig` can override the thermal zone. |
| `SimulatedSensor.hpp/cpp` | Sensor driven by a generator function (`std::function<float()>`). Uses `make_for_metric()` to select a metric-appropriate waveform. |

**Metric-specific generators** (`SimulatedSensor::make_for_metric`):

| Metric | Generator | Range |
|---|---|---|
| `temperature` | sinusoidal — base 22 °C, ±8 °C, period 120 s | 14–30 °C |
| `humidity` | sinusoidal — base 55 %, ±20 %, period 180 s | 35–75 % |
| `pressure` | sinusoidal — base 1013 hPa, ±5 hPa, period 300 s | 1008–1018 hPa |
| `motion` | periodic square wave — active 5 s every 25 s | 0.0 / 1.0 |
| *(other)* | sinusoidal — base 40, ±30, period 60 s | fallback |

**Core interface:**

```cpp
struct SensorReading {
    std::string sensor_id;
    std::string metric;   // e.g. "temperature", "pressure"
    float       value;
};

class ISensorReader {
public:
    virtual auto read()      -> std::expected<SensorReading, SensorError> = 0;
    virtual auto sensor_id() -> std::string = 0;
};
```

`std::expected` carries either a valid reading or an error code (`DeviceNotFound`, `ReadFailure`, `ParseError`) without throwing exceptions. The `metric` field identifies the physical quantity being measured.

**Adding a new sensor type:**
1. Create a class that inherits from `ISensorReader`.
2. Add it to `SensorType` in `Config.hpp`.
3. Instantiate it in the `make_sensor()` factory in `MonitoringHub.cpp`.

---

### 3.2 `events` layer

| File | Role |
|---|---|
| `SensorEvent.hpp` | Generic event: type, metric, value, threshold, sensor id, timestamp. |
| `EventBus.hpp/cpp` | Handler registry. Synchronous dispatch protected by a `std::mutex`. |

**Event types:**

| `SensorEvent::Type` | Meaning |
|---|---|
| `Reading` | Periodic sensor reading, dispatched every poll cycle (consumed by the dashboard). |
| `ThresholdExceeded` | Value crossed a threshold (warn or crit). |
| `ThresholdRecovered` | Value dropped back below `threshold − hysteresis`. |

**Event structure:**
```cpp
struct SensorEvent {
    Type        type;
    std::string metric;     // e.g. "temperature", "pressure"
    float       value;
    float       threshold;
    std::string sensor_id;
    std::chrono::system_clock::time_point timestamp;
};
```

---

### 3.3 `monitoring` layer

| File | Role |
|---|---|
| `Config.hpp` | `SensorConfig` per sensor (id, type, metric, thresholds) + global params (hysteresis, interval). |
| `MonitorConfig` (in `ThresholdMonitor.hpp`) | Flat config consumed by one `ThresholdMonitor` instance. |
| `MonitoringHub.hpp/cpp` | Reads `Config`, instantiates all sensors and monitors, owns their lifecycle. |
| `ThresholdMonitor.hpp/cpp` | Polling loop in a `std::jthread`. Compares value to thresholds and dispatches events. |
| `ConfigLoader.hpp/cpp` | Loads and validates `Config` from a JSON file. Returns `std::expected<Config, string>`. |

**Config structure:**
```cpp
struct SensorConfig {
    std::string id;
    SensorType  type;
    std::string device_path;
    std::string metric         = "temperature";
    float       threshold_warn = 60.0f;
    float       threshold_crit = 80.0f;
};

struct GpioConfig {
    bool enabled = false;
    int  pin     = 17;    // BCM pin number
};

struct Config {
    std::vector<SensorConfig> sensors;     // one entry per sensor to monitor
    float                     hysteresis;
    std::chrono::milliseconds poll_interval;
    bool                      web_enabled;
    uint16_t                  web_port;
    GpioConfig                gpio_alert;
};
```

**Hysteresis handling:**

```
value
    │      ┌──────────────── threshold_crit ──────────────────
    │      │  ← EXCEEDED dispatched                           │
    │      │                                         ← RECOVERED dispatched
    │      └──────────────── threshold_crit − hysteresis ─────
    └─ time
```

Without hysteresis, a sensor oscillating around a threshold would fire an event every cycle.
The `warn_active_` / `crit_active_` state is maintained across polling cycles within each `ThresholdMonitor`.

---

### 3.4 `alerts` layer

| File | Role |
|---|---|
| `IAlertHandler.hpp` | Interface: `on_event(const SensorEvent&)`. |
| `LogAlert.hpp/cpp` | Prints to stdout using `std::println` and `std::format`. |
| `GpioAlert.hpp/cpp` | Drives a BCM GPIO pin HIGH on `ThresholdExceeded`, LOW on `ThresholdRecovered`. Uses the Linux sysfs interface (`/sys/class/gpio/`); no external library. Gracefully degrades when the pin is unavailable (e.g. in CI). Enabled via `gpio_alert` in `config.json`. |
| `MqttPublisher.hpp/cpp` | Publishes events to an MQTT broker (HiveMQ Cloud or any broker). Enabled via `config.json`. Also subscribes to `rpi/history/req` and serves history-on-demand by querying `HistoryStore` (see [persistence.md](persistence.md)). |

**Adding a new handler:**
1. Create a class that inherits from `IAlertHandler`.
2. Register it with the `EventBus` at startup (`bus.register_handler(...)`).

---

### 3.5 `web` layer

| File | Role |
|---|---|
| `WebState.hpp/cpp` | Thread-safe in-memory state: current value, status, and up to 120 readings of history per sensor. `prime_history()` repopulates a sensor's history at startup from `HistoryStore` so `/api/state` is non-empty after a daemon restart. |
| `WebAlert.hpp/cpp` | `IAlertHandler` implementation that feeds `WebState` on each event. |
| `HttpServer.hpp/cpp` | Embedded HTTP server (cpp-httplib). Serves the dashboard HTML and JSON APIs. |

---

### 3.6 `persistence` layer

| File | Role |
|---|---|
| `HistoryStore.hpp/cpp` | SQLite-backed persistent ring buffer of sensor readings. Single table `readings(sensor_id, ts, value, metric)` with WAL journal. Provides `insert`, `recent(limit)`, `since(ts, limit)`, `metric_for`, and `rotate()` (time- and count-based). |
| `SqliteHistoryHandler.hpp/cpp` | `IAlertHandler` that writes every `Reading` event to `HistoryStore`. Threshold events are ignored. |

The DB lives at `data/history.db` by default (configurable). Retention and per-sensor row caps come from the `history` block in `config.json`. See [persistence.md](persistence.md) for the schema, rotation policy, and the MQTT history-on-demand protocol used by the cloud dashboard.

**Endpoints:**

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Dashboard HTML (Chart.js, auto-refresh every 2 s) |
| `/api/state` | GET | Current readings, history, and recent alerts (JSON) |
| `/api/config` | GET | Sensor thresholds |
| `/api/config` | POST | Update thresholds at runtime (no restart required) |

The dashboard automatically creates one card per sensor with units (`°C`, `%`, `hPa`) and renders motion as a step chart with `Detected` / `Clear` labels.

---

## 4. Simplified class diagram

```
┌──────────────────┐        ┌────────────────────────┐
│  ISensorReader   │        │     IAlertHandler      │
│  <<interface>>   │        │     <<interface>>      │
│ + read()         │        │ + on_event(event)      │
│ + sensor_id()    │        └───────────┬────────────┘
└──────┬───────────┘                    │ implements
       │ implements             ┌───────┴──────────┐
   ┌───┴──────────┐             │   LogAlert       │
   │ DS18B20Reader│             ├──────────────────┤
   ├──────────────┤             │   WebAlert       │
   │ DHT11Reader  │             ├──────────────────┤
   ├──────────────┤             │   MqttPublisher  │
   │CpuTempReader │             ├──────────────────┤
   ├──────────────┤             │   GpioAlert      │
   │SimulatedSensor│            └──────────────────┘
   └───────────────┘

┌──────────────────────────────────────┐
│           MonitoringHub              │
│ - sensors_  : vector<ISensorReader>  │
│ - monitors_ : vector<ThresholdMon.>  │
│ + MonitoringHub(bus, config)         │
│ + start() / stop()                   │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│          ThresholdMonitor            │
│ - sensor_   : ISensorReader&         │
│ - bus_      : EventBus&              │
│ - config_   : MonitorConfig          │
│ - thread_   : std::jthread           │
│ - warn_active_, crit_active_ : bool  │
│ + start() / stop()                   │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│              EventBus                │
│ - handlers_ : vector<IAlertHandler>  │
│ - mutex_    : std::mutex             │
│ + register_handler(handler)          │
│ + dispatch(SensorEvent)              │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│            HistoryStore              │
│ - db_                : sqlite3*      │
│ - retention_days_    : int           │
│ - max_points_/sensor : int           │
│ + insert(sid, metric, val, ts)       │
│ + recent(sid, limit) / since(...)    │
│ + metric_for(sid)                    │
│ + rotate()                           │
└──────────────────────────────────────┘
```

---

## 5. C++23 features used

| Feature | File(s) | Rationale |
|---|---|---|
| `std::expected<T,E>` | `ISensorReader.hpp`, `DS18B20Reader.cpp` | Exception-free error handling at the hardware boundary |
| `std::println` / `std::print` | `LogAlert.cpp`, `ThresholdMonitor.cpp` | Type-safe replacement for `printf` |
| `std::format` | `LogAlert.cpp`, `ConfigLoader.cpp` | String formatting without streams |
| `std::jthread` + `std::stop_token` | `ThresholdMonitor.cpp` | Thread with built-in cooperative stop (one per sensor) |
| Designated initializers | `main.cpp`, tests | Explicit struct initialization |
| `std::numbers::pi_v<float>` | `SimulatedSensor.cpp` | Typed mathematical constant |
