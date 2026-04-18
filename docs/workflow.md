# Workflow — Data flow and execution model

## 1. Application startup

```
main()
  │
  ├─ 1. Builds Config (thresholds, sensor type, interval)
  │
  ├─ 2. Calls make_sensor(config)
  │       └─ returns DS18B20Reader or SimulatedSensor
  │
  ├─ 3. Creates EventBus
  │
  ├─ 4. Registers handlers (LogAlert, ...)
  │       └─ bus.register_handler(...)
  │
  ├─ 5. Creates ThresholdMonitor(sensor, bus, config)
  │
  ├─ 6. monitor.start()
  │       └─ launches std::jthread → run(stop_token)
  │
  └─ 7. Main loop: waits for SIGINT / SIGTERM
          └─ monitor.stop() → joins the thread
```

---

## 2. Polling cycle (monitoring thread)

```
┌─────────────────────────────────────────────────────────┐
│  std::jthread  ::  run(stop_token)                      │
│                                                         │
│  while (!stop_requested)                                │
│    │                                                    │
│    ├─ sensor_.read()                                    │
│    │    ├─ OK  → SensorReading { temp, sensor_id }      │
│    │    └─ Err → log error, skip cycle                  │
│    │                                                    │
│    ├─ Evaluate CRITICAL threshold                       │
│    │    ├─ temp ≥ threshold_crit  &&  !crit_active_     │
│    │    │    → dispatch(ThresholdExceeded, crit)        │
│    │    │    → crit_active_ = true                      │
│    │    └─ temp < threshold_crit − hysteresis           │
│    │         → dispatch(ThresholdRecovered, crit)       │
│    │         → crit_active_ = false                     │
│    │                                                    │
│    ├─ Evaluate WARNING threshold (if !crit_active_)     │
│    │    ├─ temp ≥ threshold_warn  &&  !warn_active_     │
│    │    │    → dispatch(ThresholdExceeded, warn)        │
│    │    │    → warn_active_ = true                      │
│    │    └─ temp < threshold_warn − hysteresis           │
│    │         → dispatch(ThresholdRecovered, warn)       │
│    │         → warn_active_ = false                     │
│    │                                                    │
│    └─ sleep(poll_interval)                              │
└─────────────────────────────────────────────────────────┘
```

---

## 3. ThermalEvent lifecycle

```
ThresholdMonitor                EventBus               LogAlert
      │                             │                      │
      │──── dispatch(event) ───────►│                      │
      │                             │──on_event(event) ───►│
      │                             │                      │── std::println(...)
      │                             │◄─────────────────────│
      │◄────────────────────────────│                      │
```

Dispatch is **synchronous**: `on_event()` is called in the monitoring thread.
Handlers must therefore be non-blocking or delegate work to their own thread.

---

## 4. Threading model

```
Main thread                     Monitoring thread
      │                               │
      │  monitor.start()              │
      │ ─────────────────────────►    │  run(stop_token)
      │                               │    └─ polling loop
      │  (waiting for signal)         │
      │                               │
      │  g_running = 0 (SIGINT)       │
      │  monitor.stop()               │
      │    └─ request_stop()  ───────►│  stop_requested() = true
      │    └─ join()          ───────►│  (clean thread exit)
      │◄──────────────────────────────│
      │  return 0
```

**EventBus synchronization**: the `std::mutex` in EventBus guards concurrent access
to the handler list (registration from the main thread is safe while the monitoring
thread is dispatching).

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
For more robust handling (N consecutive errors → error event), see the possible
evolutions in section 8 of workflow.md.

---

## 6. Hysteresis — numerical example

```
Config: threshold_warn=50, threshold_crit=65, hysteresis=2

Time  Temp   warn_active  crit_active  Event dispatched
  0   45°C       false        false     —
  1   52°C       true         false     ThresholdExceeded (warn, 52°C)
  2   67°C       true         true      ThresholdExceeded (crit, 67°C)
  3   64°C       true         true      — (not yet below 65-2=63)
  4   62°C       true         false     ThresholdRecovered (crit, 62°C)
  5   49°C       true         false     — (not yet below 50-2=48)
  6   47°C       false        false     ThresholdRecovered (warn, 47°C)
```

---

## 7. Runtime sensor selection

The `make_sensor()` factory in `main.cpp` instantiates the correct type based on `Config::sensor_type`:

```
Config::sensor_type
   │
   ├─ SensorType::Simulated  →  SimulatedSensor("sim-0")
   │                               └─ sinusoidal: base=40°C, amplitude=30°C, period=60s
   │
   └─ SensorType::DS18B20    →  DS18B20Reader(config.device_path)
                                  └─ reads /sys/bus/w1/devices/<id>/temperature
```

To switch sensor: update `Config::sensor_type` and, if needed, `Config::device_path`.

---

## 8. Possible evolutions

| Need | Recommended approach |
|---|---|
| Multiple simultaneous sensors | One `ThresholdMonitor` per sensor, shared `EventBus` |
| Email / MQTT alert | New `IAlertHandler` class registered in `main()` |
| Measurement persistence | Dedicated handler writing to SQLite or InfluxDB |
| N consecutive errors → alert | Counter in `ThresholdMonitor::run()`, new enum in `ThermalEvent::Type` |
| File-based configuration | JSON/TOML parser → populate `Config` before creating the monitor |
| Real-time web interface | Handler publishing via WebSocket (e.g. Boost.Beast) |
