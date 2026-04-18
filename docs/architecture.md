# Architecture — rpi-temp-monitor

## 1. Design principles

- **Separation of concerns**: each layer only knows its immediate neighbours through abstract interfaces.
- **Testability**: the real sensor can be replaced by a simulator without modifying business logic.
- **Extensibility**: adding a sensor or an alert type only requires a new class implementing an existing interface.
- **Exception-free error handling**: `std::expected<T, E>` is used at the hardware boundary.

---

## 2. Layer view

```
┌─────────────────────────────────────────────────────────┐
│                        main.cpp                         │
│  (builds the sensor, EventBus, handlers,                │
│   ThresholdMonitor and orchestrates clean shutdown)     │
└────────────┬───────────────────────────────┬────────────┘
             │                               │
             ▼                               ▼
┌────────────────────┐           ┌───────────────────────┐
│   MONITORING       │           │   ALERTS              │
│  ThresholdMonitor  │──────────►│  IAlertHandler        │
│  Config            │  dispatch │  LogAlert             │
└────────┬───────────┘  event    └───────────────────────┘
         │                                ▲
         │ read()                         │ via EventBus
         ▼                               │
┌────────────────────┐           ┌───────────────────────┐
│   SENSORS          │           │   EVENTS              │
│  ISensorReader     │           │  ThermalEvent         │
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
class ISensorReader {
public:
    virtual auto read()      -> std::expected<SensorReading, SensorError> = 0;
    virtual auto sensor_id() -> std::string = 0;
};
```

`std::expected` carries either a valid reading or an error code (`DeviceNotFound`, `ReadFailure`, `ParseError`) without throwing exceptions.

**Adding a new sensor:**
1. Create a class that inherits from `ISensorReader`.
2. Add it to `SensorType` in `Config.hpp`.
3. Instantiate it in the `make_sensor()` factory in `main.cpp`.

---

### 3.2 `events` layer

| File | Role |
|---|---|
| `ThermalEvent.hpp` | Event data structure: type, measured temperature, crossed threshold, sensor id, timestamp. |
| `EventBus.hpp/cpp` | Handler registry. Synchronous dispatch protected by a `std::mutex`. |

**Event types:**

| `ThermalEvent::Type` | Meaning |
|---|---|
| `ThresholdExceeded` | Temperature crossed a threshold (warn or crit). |
| `ThresholdRecovered` | Temperature dropped back below `threshold − hysteresis`. |

---

### 3.3 `monitoring` layer

| File | Role |
|---|---|
| `Config.hpp` | Parameters: sensor type, warn/crit thresholds, hysteresis, polling interval. |
| `ThresholdMonitor.hpp/cpp` | Polling loop in a `std::jthread`. Compares temperature to thresholds and dispatches events. |

**Hysteresis handling:**

```
temperature
    │      ┌──────────────── threshold_crit ──────────────────
    │      │  ← EXCEEDED dispatched                           │
    │      │                                         ← RECOVERED dispatched
    │      └──────────────── threshold_crit − hysteresis ─────
    └─ time
```

Without hysteresis, a sensor oscillating around a threshold would fire an event every cycle.
The `warn_active_` / `crit_active_` state is maintained across polling cycles.

---

### 3.4 `alerts` layer

| File | Role |
|---|---|
| `IAlertHandler.hpp` | Interface: `on_event(const ThermalEvent&)`. |
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
   │ DS18B20Reader│             └──────────────┘
   ├──────────────┤
   │SimulatedSensor│
   └───────────────┘

┌──────────────────────────────────────┐
│          ThresholdMonitor            │
│ - sensor_   : ISensorReader&         │
│ - bus_      : EventBus&              │
│ - config_   : Config                 │
│ - thread_   : std::jthread           │
│ - warn_active_, crit_active_ : bool  │
│ + start() / stop()                   │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│              EventBus                │
│ - handlers_ : vector<IAlertHandler> │
│ - mutex_    : std::mutex             │
│ + register_handler(handler)          │
│ + dispatch(ThermalEvent)             │
└──────────────────────────────────────┘
```

---

## 5. C++23 features used

| Feature | File(s) | Rationale |
|---|---|---|
| `std::expected<T,E>` | `ISensorReader.hpp`, `DS18B20Reader.cpp` | Exception-free error handling at the hardware boundary |
| `std::println` / `std::print` | `LogAlert.cpp`, `ThresholdMonitor.cpp` | Type-safe replacement for `printf` |
| `std::format` | `LogAlert.cpp` | String formatting without streams |
| `std::jthread` + `std::stop_token` | `ThresholdMonitor.cpp` | Thread with built-in cooperative stop |
| Designated initializers | `main.cpp`, tests | Explicit struct initialization |
| `std::numbers::pi_v<float>` | `SimulatedSensor.cpp` | Typed mathematical constant |
