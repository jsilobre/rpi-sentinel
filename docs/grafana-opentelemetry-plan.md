# Migration Plan: Grafana Cloud + OpenTelemetry

Status: **proposed** — pending review.

## 1. Context and goals

The daemon currently ships its own visualization and history-on-demand stack:
a Chart.js dashboard served by an in-process HTTP server (`src/web/`),
plus a GitHub Pages page hydrated over MQTT (`dashboard/index.html`,
`MqttPublisher::handle_history_request`, `HistoryStore::recent`).

The goal of this migration is to replace that custom stack with a standard
observability pipeline:

- **OpenTelemetry** as the wire protocol from the daemon.
- **Grafana Cloud free tier** as the backend (Mimir for metrics,
  Loki for logs, Grafana for dashboards and alerting).
- A single **public Grafana dashboard URL** in place of the GitHub Pages
  page.

What we keep:

- `ThresholdMonitor` and the hysteresis logic — it is more expressive than
  PromQL alone, already covered by tests, and lets the daemon make alerting
  decisions independently of network availability.
- `HistoryStore` SQLite — local source of truth and offline buffer.

What we remove:

- `src/web/` (HTTP server, `WebState`, `WebAlert`).
- `dashboard/` (Chart.js page + assets).
- `.github/workflows/deploy-dashboard.yml` (Pages deploy + secret injection).
- `src/alerts/MqttPublisher.{hpp,cpp}` — entire MQTT publisher.
- `MqttConfig` block in `Config.hpp` and `config.example.json`.
- `libmosquitto` dependency and the `ENABLE_MQTT` build option.
- `web_enabled` / `web_port` config fields, `/api/state`, `/api/config`,
  and the runtime threshold POST endpoint. Runtime threshold edits are
  dropped: edit `config.json` and restart the daemon (sub-second on a Pi).

## 2. Target architecture

```
┌────────────────────────────────────────────────────────────────┐
│                       rpi-sentinel daemon                      │
│                                                                │
│  Sensors ──▶ ThresholdMonitor ──▶ EventBus                     │
│                                     │                          │
│                 ┌───────────────────┼───────────────────┐      │
│                 ▼                   ▼                   ▼      │
│            LogAlert        SqliteHistoryHandler    OtlpExporter│
│           (kept)              (kept)               (NEW)       │
│                                                       │        │
│                                          ┌────────────┘        │
│                                          ▼                     │
│                                 OpenTelemetry SDK              │
│                                 (HTTP/Protobuf)                │
└──────────────────────────────────────────┼─────────────────────┘
                                           │ OTLP/HTTPS + Bearer
                                           ▼
                          ┌────────────────────────────────┐
                          │  Grafana Cloud OTLP gateway    │
                          │  (otlp-gateway-prod-XX)        │
                          └────────────┬───────────────────┘
                                       │
                       ┌───────────────┴────────────────┐
                       ▼                                ▼
              ┌────────────────┐              ┌────────────────┐
              │ Mimir (metrics)│              │ Loki (logs)    │
              │ sensor.reading │              │ threshold      │
              │                │              │ events         │
              └────────┬───────┘              └────────┬───────┘
                       │                               │
                       └────────────┬──────────────────┘
                                    ▼
                          ┌─────────────────────┐
                          │  Grafana            │
                          │  - Dashboards       │
                          │  - Public URL       │
                          │  - Alert rules      │
                          │  - Contact points   │
                          └──────────┬──────────┘
                                     ▼
                                   email
```

No MQTT in the target architecture: `MqttPublisher` and `libmosquitto`
are removed. All external consumers of sensor data (live readings,
threshold events, history) go through Grafana Cloud.

## 3. Signal mapping

| EventBus event           | OTel signal | Destination | Shape                                                                  |
|--------------------------|-------------|-------------|------------------------------------------------------------------------|
| `Reading`                | Metric (gauge) | Mimir    | `sensor.reading` with attributes `sensor_id`, `metric`                 |
| `ThresholdExceeded`      | Log (warning/error) | Loki | Body `"threshold exceeded"`, attributes `sensor_id`, `metric`, `value`, `threshold`, `severity=warning\|critical` |
| `ThresholdRecovered`     | Log (info)  | Loki        | Body `"threshold recovered"`, same attributes                          |

Resource attributes set once at startup: `service.name=rpi-sentinel`,
`service.instance.id=<config>`, `service.version=<git describe>`,
`host.name=<gethostname>`.

Severity is derived from whether the value crosses warn or crit thresholds.

## 4. Components

### 4.1 New: `src/otel/`

```
src/otel/
├── CMakeLists.txt
├── OtlpExporter.hpp
└── OtlpExporter.cpp
```

`OtlpExporter` implements `IAlertHandler`:

```cpp
class OtlpExporter : public IAlertHandler {
public:
    explicit OtlpExporter(const OtlpConfig& cfg);
    ~OtlpExporter() override;
    void on_event(const SensorEvent& event) override;
private:
    // owns the OTel MeterProvider and LoggerProvider
    // holds one Meter and one Logger instance
    // maps SensorEvent::Type to gauge update or log record
};
```

Initialization in the constructor:

1. Build OTLP HTTP exporter pointing at `cfg.endpoint` with header
   `Authorization: Basic <base64(instance_id:token)>`.
2. Wrap in a `PeriodicExportingMetricReader` (5 s interval) and a
   `BatchLogRecordProcessor`.
3. Set them as the global `MeterProvider` / `LoggerProvider`.
4. Cache `Meter` and `Logger` handles + the gauge instrument
   (`sensor.reading`).

Shutdown (destructor): call `Shutdown()` on both providers, with a 5 s timeout,
to flush in-memory buffers.

`on_event()` is non-blocking: the SDK enqueues to its background processor.
This preserves the EventBus contract.

### 4.2 New config block

Add to `Config.hpp`:

```cpp
struct OtlpConfig {
    bool        enabled            = false;
    std::string endpoint;          // e.g. https://otlp-gateway-prod-eu-west-2.grafana.net/otlp
    std::string auth_header;       // "Basic <base64(instance:token)>" — read from env preferred
    std::string service_instance_id = "rpi-sentinel-1";
    int         export_interval_ms  = 5000;
};

struct Config {
    // existing fields...
    OtlpConfig otlp;
};
```

`config.example.json`:

```json
"otlp": {
    "enabled": false,
    "endpoint": "https://otlp-gateway-prod-eu-west-2.grafana.net/otlp",
    "auth_header_env": "GRAFANA_CLOUD_OTLP_AUTH",
    "service_instance_id": "rpi-sentinel-1",
    "export_interval_ms": 5000
}
```

The token is read from the env var named in `auth_header_env` if present
(preferred), falling back to a literal `auth_header` field for dev. The
literal form is documented as not for production.

### 4.3 Modified: `src/main.cpp`

After registering existing handlers, before `hub.start()`:

```cpp
std::shared_ptr<rpi::OtlpExporter> otlp;
if (result->otlp.enabled) {
    try {
        otlp = std::make_shared<rpi::OtlpExporter>(result->otlp);
        bus.register_handler(otlp);
    } catch (const std::exception& e) {
        std::println(stderr, "[main] OTLP exporter init failed: {}", e.what());
    }
}
```

On shutdown, `otlp.reset()` is called before `hub.stop()` returns, so the
flush completes while threads are still alive.

### 4.4 Modified: `cmake/Dependencies.cmake`

Add OpenTelemetry C++ SDK. Two paths:

- **Preferred (Ubuntu 24.04+):** `apt install libopentelemetry-cpp-dev` and
  use `find_package(opentelemetry-cpp CONFIG)`. Avoids a long FetchContent
  build.
- **Fallback (cross-compile, Pi OS):** `FetchContent` of
  `open-telemetry/opentelemetry-cpp` pinned at a release tag (e.g. `v1.15.0`),
  with `WITH_OTLP_HTTP=ON`, `WITH_OTLP_GRPC=OFF`, `BUILD_TESTING=OFF` to
  minimize footprint.

We want the **HTTP/Protobuf** exporter, not gRPC: it pulls only
`libcurl` + `protobuf` rather than the full gRPC stack, which trims
build time from ~15 min to ~3 min on a Pi.

### 4.5 Removed

| Path / symbol                                          | Reason                                |
|--------------------------------------------------------|---------------------------------------|
| `src/web/` (entire directory)                          | Replaced by Grafana                   |
| `dashboard/`                                           | Replaced by Grafana public dashboard  |
| `.github/workflows/deploy-dashboard.yml`               | No more Pages deploy                  |
| `src/alerts/MqttPublisher.{hpp,cpp}`                   | All consumers now go through Grafana  |
| `Config::mqtt`, `MqttConfig`                           | MQTT block removed                    |
| `ENABLE_MQTT` build option, `libmosquitto` dep         | No more MQTT support                  |
| `Config::web_enabled`, `Config::web_port`              | Web layer gone                        |
| `/api/state`, `/api/config`, runtime threshold POST    | Tied to web layer; restart instead    |
| `cpp-httplib` FetchContent                             | No more web server                    |

The `cpp-httplib` and `libmosquitto` dependencies in
`cmake/Dependencies.cmake` are removed; `pkg-config` is no longer needed
unless we add other pkg-config-based deps (review at implementation time).

### 4.6 Kept (no changes)

- `EventBus`, `ThresholdMonitor`, all sensors.
- `HistoryStore` and `SqliteHistoryHandler`.
- `LogAlert`.
- `nlohmann_json` (still used by `ConfigLoader`).

## 5. Grafana Cloud setup (one-shot, manual)

1. Create account at `grafana.com`, **EU region** (data residency: EU).
   The OTLP endpoint will be of the form
   `https://otlp-gateway-prod-eu-west-2.grafana.net/otlp` (the exact
   sub-region depends on the stack assignment shown in the UI).
2. In *Connections → Add new connection → OpenTelemetry (OTLP)*, copy:
   - Endpoint URL.
   - Instance ID.
   - Generate an API token, scope `MetricsPublisher` + `LogsPublisher`.
   - Compose `Basic base64(instance_id:token)`.
3. Store the auth header in:
   - On the Pi: a `systemd` unit `Environment=GRAFANA_CLOUD_OTLP_AUTH=...`
     or a `/etc/rpi-sentinel.env` file referenced by `EnvironmentFile=`.
   - In CI (if integration tests hit the cloud): GitHub Secret
     `GRAFANA_CLOUD_OTLP_AUTH`.
4. Build the dashboard:
   - One *Time series* panel per metric type (temperature, humidity,
     pressure, motion), grouped by `sensor_id` label.
   - One *Stat* panel per sensor showing the current value with thresholds
     colored from the daemon's warn/crit (manual sync).
   - One *Logs* panel filtered on
     `{service_name="rpi-sentinel"} | json`, showing threshold events
     chronologically.
5. Configure alert rules:
   - **Threshold breach (Loki):**
     `count_over_time({service_name="rpi-sentinel", event_type="threshold_exceeded"}[1m]) > 0`
     — fires on every new exceeded event, grouped by `sensor_id` so flapping
     groups by sensor.
   - **Silent sensor (Mimir):**
     `absent_over_time(sensor_reading[2m])` per `sensor_id`
     — detects daemon crash or sensor failure.
6. Configure a single **email contact point** for v1. Slack/Discord/webhook
   integrations are deferred — adding them later is a UI-only change with no
   impact on the daemon.
7. Enable *Public Dashboards* on the main dashboard, copy the public URL.
8. Update `README.md` with the public dashboard URL and a screenshot.

## 6. Phased migration

The migration is additive first, destructive second, so we can roll back at
each step.

### Phase 1 — Add OTLP export (additive, no removal)

- Add OpenTelemetry C++ SDK dependency and CMake wiring.
- Implement `OtlpExporter` and unit tests using the SDK's in-memory
  `MetricExporter` / `LogRecordExporter` test doubles (no live HTTP server
  needed in unit tests).
- Add config block, wire in `main.cpp`, default `enabled: false`.
- Validate locally against an OTel Collector in Docker (`otel/opentelemetry-collector-contrib`).
- CI: build only (no cloud calls in CI).

**Exit criteria:** with `otlp.enabled=true` and a local Collector, metrics and
logs visible in the Collector debug exporter. Web dashboard, MQTT, and
SQLite history all still work unchanged.

### Phase 2 — Connect to Grafana Cloud

- Manual Grafana Cloud setup (section 5), EU region.
- Update Pi's `config.json` and systemd unit to enable OTLP.
- Build dashboards and alert rules in Grafana UI.
- Run the daemon for ~48 h alongside the existing web dashboard, compare
  values, validate alerting cadence and email delivery.

**Exit criteria:** public dashboard URL accessible, threshold breach
alerts arrive via email within 1–2 min of the daemon dispatching the event.

### Phase 3 — Decommission custom web + MQTT stack

Single destructive phase since we are working on a feature branch and have
no production deployments depending on the legacy stack.

- Remove `src/web/`, `dashboard/`, `.github/workflows/deploy-dashboard.yml`,
  `cpp-httplib` dep.
- Remove `src/alerts/MqttPublisher.{hpp,cpp}`, `MqttConfig`, the
  `ENABLE_MQTT` build option and `libmosquitto` dep.
- Remove `web_enabled` / `web_port` / `mqtt` config blocks; clean
  `config.example.json`.
- Remove primer of `WebState` from `main.cpp`; simplify the file to:
  config → bus + handlers (Log, Sqlite, Otlp) → hub → wait → shutdown.
- Update `docs/architecture.md`, `docs/persistence.md`, `docs/workflow.md`,
  `docs/build-guide.md`, `README.md`, `CLAUDE.md`.
- New `docs/observability.md` covering OTLP setup, Grafana dashboard layout,
  alert rules, and how to add new alerts.

**Exit criteria:** build green, all tests green, daemon binary noticeably
smaller, no references to `WebState`, `HttpServer`, `MqttPublisher`,
`mosquitto`, or `httplib` left in the source tree.

`HistoryStore` SQLite is kept: it is the only data path that survives an
Internet outage, adds <1 % CPU overhead, and its removal would be a
separate decision driven by retention policy, not by this migration.

## 7. Testing strategy

### Unit
- `OtlpExporter` event-to-signal mapping: feed a `SensorEvent`, assert the
  appropriate gauge update / log record is produced. Use a fake exporter
  injected via the SDK's `MetricExporter` / `LogRecordExporter` interfaces.
- Boundary cases: SDK init failure (bad endpoint, missing token) must not
  crash the daemon; `OtlpExporter` constructor throws, `main` logs and
  continues without it.

### Integration (local)
- `docker-compose.yml` in `tests/integration/otlp/` running
  `otel/opentelemetry-collector-contrib` with a debug exporter and a file
  exporter.
- A test harness starts the daemon with OTLP enabled, posts simulated
  readings, asserts the file exporter contains the expected metrics and logs.

### Manual (cloud)
- After Phase 2, document a 30-min smoke checklist in
  `docs/observability.md`: trigger a threshold breach, verify dashboard,
  verify email, kill daemon, verify silent-sensor alert fires.

### Regression
- Existing tests (`EventBus*`, `ThresholdMonitor*`, `HistoryStore*`) remain
  unchanged and must stay green throughout. Web tests are deleted in
  Phase 3.

## 8. Configuration & secrets

- The OTLP auth header is **never** committed.
- Preferred: env var `GRAFANA_CLOUD_OTLP_AUTH`, referenced by
  `auth_header_env` in `config.json`.
- `ConfigLoader` resolves `auth_header_env` → `getenv()` → `auth_header`
  field, in that order, and rejects startup if `enabled=true` but no value
  resolved.
- `config.json` stays in `.gitignore` (already the case).
- Systemd unit gets an `EnvironmentFile=/etc/rpi-sentinel.env` (mode 0600,
  owned by the daemon user). Documented in the Pi setup guide.

## 9. Risks

| Risk                                                  | Mitigation                                                                                            |
|-------------------------------------------------------|-------------------------------------------------------------------------------------------------------|
| Free tier limits exceeded                             | 6 sensors × 1 series ≈ 6 active series vs 10 000 quota; logs quota of 50 GB/month not reachable.      |
| Internet outage drops metrics                         | OTel SDK in-memory buffer (~5 min). `HistoryStore` remains canonical local store. `LogAlert` still logs locally. |
| `opentelemetry-cpp` build time on Pi                  | Use HTTP exporter only (no gRPC). Cache via `ccache` in CI. Provide `apt` path on Ubuntu.             |
| Public dashboard feature limitations                  | Confirmed sufficient for our panels (no template variables needed; current dashboard is one fixed view per sensor type). Revisit if we add per-sensor drill-down. |
| Token leak                                            | Env-var only in production. Document rotation procedure: regenerate in Grafana UI, redeploy systemd env file. |
| Schema change for Loki labels                         | Lock label set early (`service_name`, `service_instance_id`, `sensor_id`, `metric`, `event_type`, `severity`). Adding new ones is cheap; renaming forces a query rewrite. |
| Loss of runtime threshold edit (`POST /api/config`)   | Accepted: edit `config.json` and restart the daemon (sub-second on a Pi). Document this in `README.md`. |

## 9.1 Decisions locked

- **Region:** Grafana Cloud EU.
- **MQTT:** removed entirely (no live-publish kept).
- **Notifications v1:** email contact point only.
- **Runtime threshold edits:** removed; config-edit + restart instead.
- **`HistoryStore`:** kept as local source of truth and offline buffer.

## 10. Documentation updates (Phase 3)

| File                                  | Change                                                                |
|---------------------------------------|-----------------------------------------------------------------------|
| `docs/architecture.md`                | Replace web + MQTT layer description with OTel exporter; new component table |
| `docs/persistence.md`                 | Remove all MQTT/history-on-demand content; SQLite is now local-only   |
| `docs/workflow.md`                    | Update per-cycle diagram: drop `WebAlert` and `MqttPublisher`, add OTLP path |
| `docs/build-guide.md`                 | OpenTelemetry SDK install; remove `cpp-httplib`, `libmosquitto`, `ENABLE_MQTT` |
| `docs/observability.md` (new)         | OTLP setup, dashboard layout, alert rules, smoke checklist            |
| `README.md`                           | Public Grafana dashboard URL + screenshot; drop GitHub Pages and MQTT sections; document config-edit-and-restart for threshold changes |
| `CLAUDE.md`                           | Rewrite web + MQTT sections; update build prerequisites               |

## 11. Effort estimate

Rough sizing, assuming familiarity with the codebase and one focused day per phase:

- Phase 1 (OTLP exporter + tests): ~1.5 days
- Phase 2 (Grafana Cloud setup + dashboards + alerts): ~0.5 day
- Phase 3 (decommission): ~0.5 day
- Documentation: ~0.5 day

Total: **~3 days** of focused work. Most of the risk concentrates in Phase 1
around the SDK build configuration on the Pi.
