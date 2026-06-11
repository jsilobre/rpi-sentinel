# Documentation — rpi-sentinel

Technical reference for current and future development.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Components, layers, interfaces, dependencies |
| [workflow.md](workflow.md) | Data flow, event lifecycle, threading model |
| [build-guide.md](build-guide.md) | Build, tests, CI/CD, RPi deployment |
| [persistence.md](persistence.md) | History storage (SQLite), MQTT history-on-demand |
| [../companion/](../companion/) | Claude usage publisher — Pi-side sidecar that publishes Claude Code usage to the same broker for `rpi-sentinel-display` |

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
                               EventBus ──► LogAlert / SqliteHistoryHandler / MqttPublisher
```

The project is structured into **4 independent layers** (`sensors`, `events`, `monitoring`,
`alerts`, `persistence`) connected through abstract interfaces. `MonitoringHub` orchestrates
N sensors and monitors from the JSON config — `main.cpp` only bootstraps and waits for a
shutdown signal.

## Companion tooling

[`companion/`](../companion/) holds Pi-side tooling that lives alongside the daemon but is
not part of the C++ build. It currently contains the **Claude usage publisher**: a small
standalone Python sidecar that polls Anthropic's OAuth usage endpoint (account-wide, the
same data as Claude Code's `/usage` screen)
and publishes it (retained, QoS 1) to `<MQTT_PREFIX>/claude/usage` on the same broker the
daemon uses. The `rpi-sentinel-display` ESP32 subscribes to that topic to render its
**Claude usage** page — no Claude credential ever touches the display. See
[companion/README.md](../companion/README.md) for setup and the payload contract.
