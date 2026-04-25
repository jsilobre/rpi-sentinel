# Documentation — rpi-sentinel

Technical reference for current and future development.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Components, layers, interfaces, dependencies |
| [workflow.md](workflow.md) | Data flow, event lifecycle, threading model |
| [build-guide.md](build-guide.md) | Build, tests, CI/CD, RPi deployment |
| [persistence.md](persistence.md) | History storage (SQLite), MQTT history-on-demand |

## Quick overview

```
config.json
      │
      ▼
 MonitoringHub ──creates──► ThresholdMonitor[0] ──(jthread)──► ISensorReader[0]
               ──creates──► ThresholdMonitor[1] ──(jthread)──► ISensorReader[1]
               ...
                                    │
                                    │ dispatch(SensorEvent)
                                    ▼
                               EventBus ──► LogAlert / WebAlert / SqliteHistoryHandler / MqttPublisher
```

The project is structured into **5 independent layers** (`sensors`, `events`, `monitoring`,
`alerts`, `persistence`) connected through abstract interfaces. `MonitoringHub` orchestrates
N sensors and monitors from the JSON config — `main.cpp` only bootstraps, primes the web
state from persisted history, and waits for a shutdown signal.
