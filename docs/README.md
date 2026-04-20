# Documentation — rpi-sentinel

Technical reference for current and future development.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Components, layers, interfaces, dependencies |
| [workflow.md](workflow.md) | Data flow, event lifecycle, threading model |
| [build-guide.md](build-guide.md) | Build, tests, CI/CD, RPi deployment |

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
                               EventBus ──► LogAlert / WebAlert / ...
```

The project is structured into **4 independent layers** (`sensors`, `events`, `monitoring`, `alerts`)
connected through abstract interfaces. `MonitoringHub` orchestrates N sensors and monitors
from the JSON config — `main.cpp` only bootstraps and waits for a shutdown signal.
