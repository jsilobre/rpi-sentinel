# Workflow — Data flow and execution model

## 1. Application startup

```
main()
  │
  ├─ 1. Loads Config from JSON
  │       └─ list of SensorConfig + global params (hysteresis, interval, web)
  │
  ├─ 2. Creates EventBus
  │
  ├─ 3. Registers handlers
  │       └─ bus.register_handler(LogAlert)
  │       └─ bus.register_handler(WebAlert)
  │
  ├─ 4. Creates MonitoringHub(bus, config)
  │       └─ for each SensorConfig:
  │            ├─ make_sensor(sc) → DS18B20Reader or SimulatedSensor
  │            └─ ThresholdMonitor(sensor, bus, MonitorConfig{...})
  │
  ├─ 5. hub.start()
  │       └─ for each ThresholdMonitor: monitor.start()
  │            └─ launches std::jthread → run(stop_token)
  │
  ├─ 6. HttpServer::start()  (if web_enabled)
  │
  └─ 7. Main loop: waits for SIGINT / SIGTERM
          └─ hub.stop() → stops all monitors
```

---

## 2. Polling cycle (per monitoring thread)

Each `ThresholdMonitor` runs its own `std::jthread`, polling its dedicated sensor independently.

```
┌─────────────────────────────────────────────────────────┐
│  std::jthread  ::  run(stop_token)                      │
│                                                         │
│  while (!stop_requested)                                │
│    │                                                    │
│    ├─ sensor_.read()                                    │
│    │    ├─ OK  → SensorReading { value, metric, id }    │
│    │    └─ Err → log error, skip cycle                  │
│    │                                                    │
│    ├─ dispatch(Reading event)   ← always (dashboard)    │
│    │                                                    │
│    ├─ Evaluate CRITICAL threshold                       │
│    │    ├─ value ≥ threshold_crit  &&  !crit_active_    │
│    │    │    → dispatch(ThresholdExceeded, crit)        │
│    │    │    → crit_active_ = true                      │
│    │    └─ value < threshold_crit − hysteresis          │
│    │         → dispatch(ThresholdRecovered, crit)       │
│    │         → crit_active_ = false                     │
│    │                                                    │
│    ├─ Evaluate WARNING threshold (if !crit_active_)     │
│    │    ├─ value ≥ threshold_warn  &&  !warn_active_    │
│    │    │    → dispatch(ThresholdExceeded, warn)        │
│    │    │    → warn_active_ = true                      │
│    │    └─ value < threshold_warn − hysteresis          │
│    │         → dispatch(ThresholdRecovered, warn)       │
│    │         → warn_active_ = false                     │
│    │                                                    │
│    └─ sleep(poll_interval)                              │
└─────────────────────────────────────────────────────────┘
```

---

## 3. SensorEvent lifecycle

```
ThresholdMonitor[N]             EventBus               LogAlert / WebAlert
      │                             │                          │
      │──── dispatch(event) ───────►│                          │
      │                             │──on_event(event) ────────►│
      │                             │                          │── std::println(...)
      │                             │◄─────────────────────────│
      │◄────────────────────────────│                          │
```

Dispatch is **synchronous**: `on_event()` is called in the monitoring thread.
Handlers must therefore be non-blocking or delegate work to their own thread.

All monitors share the same `EventBus`, so events from different sensors interleave
through the same handler chain and are distinguished by `event.sensor_id` and `event.metric`.

---

## 4. Threading model

```
Main thread          Monitor thread [0]      Monitor thread [1]
      │                     │                       │
      │  hub.start()        │                       │
      │ ──────────────────► │  run(stop_token)      │  run(stop_token)
      │                     │    └─ polling loop    │    └─ polling loop
      │  (waiting for signal)
      │
      │  g_running = 0 (SIGINT)
      │  hub.stop()
      │    └─ request_stop() ──────────────────────►│×N  stop_requested() = true
      │    └─ join()          ──────────────────────►│×N  (clean thread exit)
      │◄────────────────────────────────────────────
      │  return 0
```

**EventBus synchronization**: the `std::mutex` in `EventBus` serializes concurrent dispatches
from multiple monitoring threads and protects handler registration from the main thread.

---

## 5. Sensor read error handling

```
sensor_.read() returns std::unexpected(SensorError::...)
         │
         ▼
ThresholdMonitor::run()
  └─ std::println("[ThresholdMonitor] read error: {}", ...)
  └─ skip → sleep → retry on next cycle
```

Transient errors (e.g. 1-Wire bus noise) are silently skipped.
The `Reading` event is not dispatched on error cycles, so the dashboard simply
stops receiving updates for that sensor until reads succeed again.

---

## 6. Hysteresis — numerical example

```
Config: threshold_warn=50, threshold_crit=65, hysteresis=2

Time  Value  warn_active  crit_active  Event dispatched
  0     45       false        false     —
  1     52       true         false     ThresholdExceeded (warn, 52)
  2     67       true         true      ThresholdExceeded (crit, 67)
  3     64       true         true      — (not yet below 65-2=63)
  4     62       true         false     ThresholdRecovered (crit, 62)
  5     49       true         false     — (not yet below 50-2=48)
  6     47       false        false     ThresholdRecovered (warn, 47)
```

---

## 7. Runtime sensor instantiation

`MonitoringHub` contains a `make_sensor()` factory that instantiates the correct sensor
type based on each `SensorConfig`:

```
SensorConfig::type
   │
   ├─ SensorType::Simulated  →  SimulatedSensor(id, metric)
   │                               └─ sinusoidal: base=40, amplitude=30, period=60s
   │
   └─ SensorType::DS18B20    →  DS18B20Reader(device_path, id, metric)
                                  └─ reads /sys/bus/w1/devices/<id>/temperature
```

To add a new sensor to a running config: add an entry to the `sensors` array in `config.json`
and restart the process.

---

## 8. Possible evolutions

| Need | Recommended approach |
|---|---|
| Email / MQTT alert | New `IAlertHandler` class registered in `main()` |
| Measurement persistence | Dedicated handler writing to SQLite or InfluxDB |
| N consecutive errors → alert | Counter in `ThresholdMonitor::run()`, new enum in `SensorEvent::Type` |
| Per-sensor dashboard charts | Index `WebState::history_` by `sensor_id` |
| Dynamic config reload (no restart) | `MonitoringHub::reload(Config)` with stop/restart of affected monitors |
| Rate-of-change alert | New monitor type implementing `ISensorReader` + derivative logic |
