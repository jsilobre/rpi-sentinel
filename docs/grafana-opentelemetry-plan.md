# Migration Plan: Grafana Cloud + OpenTelemetry

## Current status

| Phase | Status | Notes |
|---|---|---|
| 1. OTLP export (additive) | **Done** | `src/otel/OtlpExporter`, gated behind `ENABLE_OTLP=ON` (default OFF), 4 unit tests, 33/33 green with OTLP, 29/29 without. Dashboard JSON shipped. Commits `f0fdda9`, `dedc8dd`. |
| 2. Connect to Grafana Cloud | **In progress** | Account created (EU), token validated against `otlp-gateway-prod-eu-west-2` (HTTP 200 round-trip). Pi build pending; dashboard to import once metrics flow. Procedure in `docs/observability.md`. |
| 3. Decommission web + MQTT  | **Not started** | Deferred until Phase 2 fully validated in production. |

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

| EventBus event / source  | OTel signal | Destination | Shape                                                                  |
|--------------------------|-------------|-------------|------------------------------------------------------------------------|
| `Reading`                | Observable Gauge | Mimir | `sensor.reading` with attributes `sensor_id`, `metric` (sampled from a thread-safe map of latest values) |
| `cfg.sensors[].threshold_warn` (static) | Observable Gauge | Mimir | `sensor.threshold.warn` with `sensor_id`, `metric` — overlays on the dashboard as a dashed warn line |
| `cfg.sensors[].threshold_crit` (static) | Observable Gauge | Mimir | `sensor.threshold.crit` with `sensor_id`, `metric` — overlays as a dashed crit line |
| `ThresholdExceeded`      | Log (warning) | Loki      | Body `"threshold_exceeded"`, attributes `sensor_id`, `metric`, `value`, `threshold`, `event_type`, `severity` |
| `ThresholdRecovered`     | Log (info)  | Loki        | Body `"threshold_recovered"`, same attribute set |

The two `sensor.threshold.*` gauges allow the dashboard to render warn/crit
overlay lines per sensor **without** the dashboard knowing the sensor list.
This keeps the dashboard fully generic — see §5.

Resource attributes set once at startup: `service.name=rpi-sentinel`,
`service.instance.id` (from config), `host.name` (from `gethostname`).

`ABI v1` of opentelemetry-cpp is used throughout (the synchronous `Gauge`
instrument is gated behind ABI v2 / experimental, so we use
`ObservableGauge` with callbacks instead).

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
    OtlpExporter(const OtlpConfig& cfg, std::span<const SensorConfig> sensors);
    ~OtlpExporter() override;
    void on_event(const SensorEvent& event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // owns Meter/Logger handles, gauges, latest-value table
};
```

`sensors` is captured at construction so per-sensor warn/crit thresholds
can be exposed as observable gauges. They are static for the daemon's
lifetime (runtime threshold edits are not supported — see §9.1).

Initialization in the constructor:

1. Resolve auth header from env (`auth_header_env`) → fallback `auth_header`.
   Throws if both are empty.
2. Build OTLP HTTP exporters (metrics + logs), HTTP/Protobuf,
   `Authorization: Basic <base64(instance_id:token)>` header, separate URLs
   `<endpoint>/v1/metrics` and `<endpoint>/v1/logs`.
3. Wrap in `PeriodicExportingMetricReader` (configurable interval, default
   5 s) and `BatchLogRecordProcessor` (2 s schedule delay).
4. Install global `MeterProvider` / `LoggerProvider`.
5. Create three observable gauges (`sensor.reading`,
   `sensor.threshold.warn`, `sensor.threshold.crit`) and register their
   callbacks against an internal thread-safe table (readings) and the
   captured sensor list (thresholds).

Shutdown (destructor): remove gauge callbacks, then reset the global
providers — destruction triggers flush + drain on the SDK's batch
processors.

`on_event()` is non-blocking: `Reading` updates the in-memory map under
a short mutex; threshold events enqueue a log record to the SDK's batch
processor. Both return immediately, preserving the EventBus contract.

### 4.2 New config block

Add to `Config.hpp`:

```cpp
struct OtlpConfig {
    bool        enabled              = false;
    std::string endpoint;            // e.g. https://otlp-gateway-prod-eu-west-2.grafana.net/otlp
    std::string auth_header;         // literal "Basic <base64(...)>" — dev-only fallback
    std::string auth_header_env      = "GRAFANA_CLOUD_OTLP_AUTH"; // env var name (preferred)
    std::string service_instance_id  = "rpi-sentinel-1";
    int         export_interval_ms   = 5000;
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
#ifdef ENABLE_OTLP
std::shared_ptr<rpi::OtlpExporter> otlp_exporter;
if (result->otlp.enabled) {
    try {
        otlp_exporter = std::make_shared<rpi::OtlpExporter>(result->otlp, result->sensors);
        bus.register_handler(otlp_exporter);
    } catch (const std::exception& e) {
        std::println(stderr, "[main] OTLP exporter init failed: {}", e.what());
        otlp_exporter.reset();
    }
}
#else
if (result->otlp.enabled) {
    std::println(stderr, "[main] otlp.enabled=true but binary built without ENABLE_OTLP — ignored");
}
#endif
```

On shutdown, `otlp_exporter.reset()` is called after `hub.stop()` returns
so the SDK can drain the final batch while no more events are produced.

### 4.4 Modified: `cmake/Dependencies.cmake`

OpenTelemetry C++ SDK is integrated via **FetchContent** pinned to
`v1.18.0`, behind a CMake option:

```cmake
option(ENABLE_OTLP "Enable OTLP/HTTP export to Grafana Cloud" OFF)
```

Default OFF so CI and local iteration stay fast (the SDK is the slowest
dep in the project). Enable on actual deployments with `-DENABLE_OTLP=ON`.

Configuration choices:

- `WITH_OTLP_HTTP=ON`, `WITH_OTLP_GRPC=OFF` — HTTP/Protobuf only.
  Pulls `libcurl` + `protobuf`, not the full gRPC stack.
- `WITH_ABSEIL=OFF`, `WITH_EXAMPLES=OFF`, `OPENTELEMETRY_INSTALL=OFF`,
  `BUILD_W3CTRACECONTEXT_TEST=OFF` — minimize footprint.
- **`BUILD_TESTING` workaround:** opentelemetry-cpp's CMakeLists keys
  off `BUILD_TESTING` for its own tests **and** FORCE-sets it to OFF
  in the cache mid-`MakeAvailable`. We save the user value before
  FetchContent and restore it after, so `tests/` still builds.
- **SYSTEM include trick:** `target_include_directories(otel SYSTEM PRIVATE ...)`
  re-adds the SDK headers as system headers so the project's
  `-Wshadow -Wpedantic -Werror` does not trip on third-party code.

Apt prerequisites on Debian / Raspberry Pi OS / Ubuntu 24.04 when
building with `ENABLE_OTLP=ON`:

```
sudo apt-get install -y \
    g++-14 cmake ninja-build libsqlite3-dev \
    libprotobuf-dev protobuf-compiler \
    libcurl4-openssl-dev libssl-dev zlib1g-dev
```

Note: `libprotobuf-dev` and `protobuf-compiler` are **separate** packages
in Debian. Installing only the first leads to
`PROTOBUF_PROTOC_EXECUTABLE-NOTFOUND` at configure time. If that error
shows up after a partial install, add the missing package and **delete
`build/`** before re-running CMake (cached not-found values stick).

The first build of `opentelemetry-cpp` takes ~3 min on x86_64 and
**15–25 min on a Pi 4/5**. Subsequent builds are fully incremental
(seconds). Run the first build under `screen` / `tmux` if connected
over SSH.

A native apt package (`libopentelemetry-cpp-dev`) is **not** available
on noble or Bookworm at the time of writing; FetchContent is the only
reasonable path.

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

The full step-by-step procedure (account creation, OTLP token, systemd
env file, dashboard import, alert rules, smoke checklist, troubleshooting)
lives in `docs/observability.md`. Key choices made during this design:

### Dashboard: a single generic JSON, not per-config-generated

A `dashboards/rpi-sentinel.json` file is shipped in the repo. It is
**fully sensor-agnostic** — the user imports it once and never touches
it when sensors change.

How it works:
- Template variable `$sensor_id = label_values(sensor_reading, sensor_id)`
  populates a multi-select dropdown from the data Mimir has actually
  ingested. Adding a sensor to `config.json` makes it appear in the
  dropdown automatically.
- A single time series panel is configured with `repeat: $sensor_id`.
  Grafana clones it once per selected sensor at render time.
- Each cloned panel overlays three queries: `sensor_reading`,
  `sensor_threshold_warn`, `sensor_threshold_crit`, all filtered by
  `sensor_id="$sensor_id"`. The threshold series come from the static
  observable gauges described in §3 — they render as dashed warn/crit
  lines without the dashboard knowing the threshold values.
- A Loki logs panel filtered on `event_type=~"threshold_.*"` shows
  breach / recovery events, also filterable via `$sensor_id`.

This avoids the alternatives we considered and rejected:
- **In-daemon provisioning** (POST dashboard JSON to `/api/dashboards/db`)
  — would need a second Grafana token with `Editor` scope and ~300 lines
  of dashboard JSON generation in C++. Disproportionate.
- **External generator script** reading `config.json` to build the
  dashboard JSON — needed re-import on every sensor change.
- **GitOps tools (Grizzly, Terraform)** — overkill for one dashboard,
  one environment.

### Alerts: email-only, one breach rule + one silent-sensor rule

Configured in *Alerting → Alert rules* (full PromQL/LogQL queries,
folder layout and contact-point setup in `docs/observability.md`).
The two rules complement the daemon's hysteresis: it owns the
"is this a real breach" decision; Grafana owns notification delivery
and the "is the daemon alive" check.

### Public dashboard

Optional, deferred. Note: Grafana Cloud public dashboards do **not**
support template variables. To publish, either remove `$sensor_id`
(and switch repeat to `metric`) or accept that the public version is a
trimmed copy. Decision postponed to after Phase 2 validates the
authenticated dashboard.

## 6. Phased migration

The migration is additive first, destructive second, so we can roll back at
each step.

### Phase 1 — Add OTLP export (additive, no removal) — **DONE**

Delivered:

- `cmake/Dependencies.cmake`: `ENABLE_OTLP` option (default OFF) + FetchContent of
  opentelemetry-cpp v1.18.0 (HTTP/Protobuf only, no gRPC). `BUILD_TESTING`
  save-restore workaround. Apt prerequisites documented in §4.4.
- `src/otel/OtlpExporter.{hpp,cpp}` implementing `IAlertHandler`. Three
  observable gauges (`sensor.reading`, `sensor.threshold.warn`,
  `sensor.threshold.crit`), batch log processor for threshold events.
  Resource attributes: `service.name`, `service.instance.id`, `host.name`.
- `src/main.cpp`: instantiation + flush on shutdown, gated `#ifdef ENABLE_OTLP`.
- `OtlpConfig` in `Config.hpp`, parsed by `ConfigLoader`, serialized by
  `save_config`. Auth resolved via env var (`auth_header_env` →
  `getenv` → literal `auth_header` fallback).
- `dashboards/rpi-sentinel.json`: generic dashboard (template vars +
  repeat panels + threshold overlay).
- `docs/observability.md`: end-to-end setup, dashboard import, alert
  rules, smoke checklist, troubleshooting.
- 4 ConfigLoader tests for the otlp block (always run).
- 4 OtlpExporter constructor tests (gated on `ENABLE_OTLP`).

Verification:
- `ENABLE_OTLP=OFF` (CI default path): 29/29 tests pass, build under 1 min.
- `ENABLE_OTLP=ON`: 33/33 tests pass; SDK build ~3 min on x86_64,
  ~15-25 min on a Pi 4/5.

Commits: `f0fdda9` (scaffolding), `dedc8dd` (threshold gauges + dashboard).

### Phase 2 — Connect to Grafana Cloud — **IN PROGRESS**

- [x] Grafana Cloud account created, EU region.
- [x] OTLP token generated (scopes `MetricsPublisher` + `LogsPublisher`),
      validated against `otlp-gateway-prod-eu-west-2` via `curl` (HTTP 200).
- [x] `/etc/rpi-sentinel.env` deployed with `GRAFANA_CLOUD_OTLP_AUTH`.
- [ ] Pi build with `ENABLE_OTLP=ON` (in progress; first build ~15–25 min
      due to SDK compile from source).
- [ ] systemd service file installed and enabled.
- [ ] Daemon log shows `[otlp] Exporter initialized`, metrics visible in
      *Explore → Mimir* on `sensor_reading` query.
- [ ] `dashboards/rpi-sentinel.json` imported, datasource mapping done.
- [ ] Two alert rules created (threshold breach + silent sensor) with
      email contact point.
- [ ] Smoke test from `docs/observability.md` executed end to end.

**Exit criteria:** authenticated dashboard URL accessible, threshold breach
alerts arrive via email within 1–2 min of the daemon dispatching the event.
Web dashboard, MQTT, and SQLite history all still work unchanged
(removal happens in Phase 3).

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

### Unit (delivered)
- `tests/test_config_loader.cpp`: 4 tests for the `otlp` block — defaults,
  full block parsing, `enabled=true` requires endpoint, non-positive
  `export_interval_ms` rejected. Always run, no SDK dependency.
- `tests/test_otlp_exporter.cpp` (gated `ENABLE_OTLP`): 4 tests —
  rejects empty endpoint, rejects missing auth, constructs and accepts
  all event types without throwing, resolves auth header from env.
  Uses an unroutable endpoint and a long export interval so no real
  network traffic is generated.

### Integration (deferred, optional)
- A `docker-compose.yml` running `otel/opentelemetry-collector-contrib`
  with a file exporter would let us assert the on-wire format end to end
  without hitting Grafana Cloud. Not implemented in Phase 1 — the
  `curl` smoke test against the real gateway plus the constructor
  unit tests have proven sufficient so far.

### Manual (cloud)
- Smoke checklist in `docs/observability.md` §"Smoke checklist after
  Phase 2 deployment": trigger a threshold breach, verify dashboard,
  verify email, kill daemon, verify silent-sensor alert fires.

### Regression
- Existing tests (`EventBus*`, `ThresholdMonitor*`, `HistoryStore*`,
  `ConfigLoader*`, `WebStateHydration*`) remain green throughout
  Phase 1. Web tests are deleted in Phase 3.

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

## 11. Effort estimate vs actuals

| Phase | Estimate | Actual | Notes |
|---|---|---|---|
| Phase 1 (OTLP exporter + tests) | ~1.5 days | ~1 day | Most time spent on SDK API quirks: ABI v1 vs v2 (synchronous Gauge gated), `BUILD_TESTING` collision, `-Wshadow` in third-party headers. All resolved. |
| Phase 1 extension (threshold gauges + dashboard) | not estimated | ~0.5 day | Added after deciding the dashboard should be fully generic (§5). |
| Phase 2 (Grafana Cloud + dashboards + alerts) | ~0.5 day | in progress | Account creation and token validation went smoothly; SDK build on Pi is the long pole (~15–25 min one-shot). |
| Phase 3 (decommission) | ~0.5 day | not started | Triggered after Phase 2 validated in production. |
| Documentation | ~0.5 day | ~0.5 day so far | `docs/observability.md` written; remaining doc updates are part of Phase 3. |

Risks materialized so far: the SDK build on Raspberry Pi is exactly as
slow as expected; mitigation (gating behind `ENABLE_OTLP` and using
HTTP-only) pays off in CI iteration speed.
