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
│  (loads config, creates EventBus + handlers +           │
│   MonitoringHub, waits for shutdown signal)             │
└────────────┬───────────────────────────────┬────────────┘
             │                               │
             ▼                               ▼
┌────────────────────────┐       ┌───────────────────────┐
│   MONITORING           │       │   ALERTS              │
│  MonitoringHub         │──────►│  IAlertHandler        │
│  ThresholdMonitor [×N] │  via  │  LogAlert             │
│  Config / MonitorConfig│  bus  │  WebAlert             │
└────────┬───────────────┘       └───────────────────────┘
         │                                ▲
         │ read()                         │ via EventBus
         ▼                               │
┌────────────────────┐           ┌───────────────────────┐
│   SENSORS          │           │   EVENTS              │
│  ISensorReader     │           │  SensorEvent          │
│  DS18B20Reader     │           │  EventBus             │
│  SimulatedSensor   │           └───────────────────────┘
└────────────────────┘
```

### Dependency rule

```
main → monitoring → sensors
                  → events → alerts
```

No lower layer depends on a higher layer.

---

## 3. Components

### 3.1 `sensors` layer

| File | Role |
|---|---|
| `ISensorReader.hpp` | Abstract interface. Defines `read()` and `sensor_id()`. |
| `DS18B20Reader.hpp/cpp` | Reads `/sys/bus/w1/devices/<id>/temperature` (Linux 1-Wire kernel driver). |
| `SimulatedSensor.hpp/cpp` | Sensor driven by a generator function (`std::function<float()>`). Defaults to a sinusoidal wave. |

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

struct Config {
    std::vector<SensorConfig> sensors;     // one entry per sensor to monitor
    float                     hysteresis;
    std::chrono::milliseconds poll_interval;
    bool                      web_enabled;
    uint16_t                  web_port;
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

**Adding a new handler:**
1. Create a class that inherits from `IAlertHandler`.
2. Register it with the `EventBus` at startup (`bus.register_handler(...)`).

Possible examples: sending email (SMTP), writing to a database, toggling a GPIO, MQTT notification.

---

## 4. Simplified class diagram

```
┌──────────────────┐        ┌────────────────────────┐
│  ISensorReader   │        │     IAlertHandler      │
│  <<interface>>   │        │     <<interface>>      │
│ + read()         │        │ + on_event(event)      │
│ + sensor_id()    │        └───────────┬────────────┘
└──────┬───────────┘                    │ implements
       │ implements             ┌───────┴──────┐
   ┌───┴──────────┐             │   LogAlert   │
   │ DS18B20Reader│             ├──────────────┤
   ├──────────────┤             │   WebAlert   │
   │SimulatedSensor│            └──────────────┘
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
