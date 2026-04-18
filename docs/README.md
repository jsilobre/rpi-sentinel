# Documentation — rpi-temp-monitor

Technical reference for current and future development.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Components, layers, interfaces, dependencies |
| [workflow.md](workflow.md) | Data flow, event lifecycle, threading model |
| [build-guide.md](build-guide.md) | Build, tests, CI/CD, RPi deployment |

## Quick overview

```
Physical / simulated sensor
        │
        ▼
  ThresholdMonitor  ──(std::jthread)──►  periodic reading
        │
        ▼  (threshold crossed)
    EventBus  ──►  LogAlert / future handlers
```

The project is structured into **4 independent layers** (`sensors`, `events`, `monitoring`, `alerts`)
connected through abstract interfaces, making it easy to add a new sensor or a new alert type
without touching the rest of the code.
