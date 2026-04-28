# Observability — Grafana Cloud + OpenTelemetry

The daemon ships sensor data over OTLP/HTTP to Grafana Cloud. Metrics
land in Mimir, threshold events land in Loki, and a single generic
dashboard renders both. No code change is required when sensors are
added or removed in `config.json` — the dashboard auto-discovers them
from labels.

## Signals emitted by the daemon

| OTel signal              | Prometheus / Loki name        | Labels                              | When                                 |
|--------------------------|-------------------------------|-------------------------------------|--------------------------------------|
| Gauge `sensor.reading`   | `sensor_reading`              | `sensor_id`, `metric`               | Every poll cycle, observed at export |
| Gauge `sensor.threshold.warn` | `sensor_threshold_warn`  | `sensor_id`, `metric`               | Static, from `config.json`           |
| Gauge `sensor.threshold.crit` | `sensor_threshold_crit`  | `sensor_id`, `metric`               | Static, from `config.json`           |
| Log warning              | (Loki, `service_name="rpi-sentinel"`, `event_type="threshold_exceeded"`) | `sensor_id`, `metric`, `value`, `threshold`, `severity` | On `ThresholdExceeded` event |
| Log info                 | (Loki, `service_name="rpi-sentinel"`, `event_type="threshold_recovered"`) | same | On `ThresholdRecovered` event |

Resource attributes attached to every signal: `service.name=rpi-sentinel`,
`service.instance.id` (from config), `host.name` (from `gethostname`).

## Build & run

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 \
              -DCMAKE_BUILD_TYPE=Release \
              -DENABLE_OTLP=ON \
              -DBUILD_TESTING=ON
cmake --build build --parallel
```

The `ENABLE_OTLP` option pulls `opentelemetry-cpp` v1.18.0 (HTTP/Protobuf
exporter only — no gRPC). The first configure adds ~2 minutes of build
time; subsequent builds are incremental.

## Grafana Cloud setup (one-shot)

1. Create a free account on `grafana.com`, **EU region**.
2. *Connections → Add new connection → OpenTelemetry (OTLP)*. Note:
   - Endpoint URL (e.g. `https://otlp-gateway-prod-eu-west-2.grafana.net/otlp`).
   - Instance ID.
   - Generate an API token with scopes `MetricsPublisher` + `LogsPublisher`.
3. Compose the auth header:
   ```bash
   AUTH=$(printf '%s:%s' "$INSTANCE_ID" "$TOKEN" | base64 -w0)
   echo "Basic $AUTH"
   ```
4. Configure the Pi to expose the auth header before launching the daemon.
   With systemd:
   ```
   # /etc/rpi-sentinel.env  (mode 0600)
   GRAFANA_CLOUD_OTLP_AUTH=Basic <base64>
   ```
   ```
   # /etc/systemd/system/rpi-sentinel.service
   [Service]
   EnvironmentFile=/etc/rpi-sentinel.env
   ExecStart=/usr/local/bin/rpi-sentinel /etc/rpi-sentinel/config.json
   ```
5. In `config.json`, enable OTLP:
   ```json
   "otlp": {
     "enabled": true,
     "endpoint": "https://otlp-gateway-prod-eu-west-2.grafana.net/otlp",
     "auth_header_env": "GRAFANA_CLOUD_OTLP_AUTH",
     "service_instance_id": "rpi-sentinel-1",
     "export_interval_ms": 5000
   }
   ```
6. Start the daemon, watch for `[otlp] Exporter initialized` in the logs.

The first metric/log records reach Grafana Cloud within
`export_interval_ms` (metrics) and ~2 s (logs).

## Importing the dashboard

The repo ships `grafana/dashboards/rpi-sentinel.json` — a single generic dashboard
that works for any sensor configuration.

1. In Grafana Cloud, *Dashboards → New → Import*.
2. *Upload JSON file* → pick `grafana/dashboards/rpi-sentinel.json`.
3. Select the **Prometheus** datasource (named `grafanacloud-<stack>-prom`)
   for `${DS_PROMETHEUS}`, and the **Loki** datasource for `${DS_LOKI}`.
4. *Import*.

What you get:

- A `Sensor` dropdown at the top, populated from
  `label_values(sensor_reading, sensor_id)`. Multi-select; defaults to All.
- A time series panel **per selected sensor** (Grafana repeats the panel
  template for each `$sensor_id`). Each panel overlays:
  - The reading itself (solid line, color from the Grafana palette).
  - A dashed yellow line at `sensor_threshold_warn`.
  - A dashed red line at `sensor_threshold_crit`.
- A logs panel showing all threshold breach / recovery events, filtered
  by the same `Sensor` selection.

When you add or remove a sensor in `config.json` and restart the daemon,
the dropdown picks up the change automatically — no dashboard edit.

### Optional: make the dashboard public

Once happy with the layout, *Dashboard settings → Public dashboard →
Generate public URL*. The link is read-only, no login required, and can
be revoked at any time. Note: public dashboards on Grafana Cloud do not
support template variables — you'll need to remove the `$sensor_id`
variable and switch repeat to `metric` (or pre-pick a sensor list) for
the public version. Keep the templated version for authenticated use.

## Alert rules

Two recommended rules. Configure them in *Alerting → Alert rules → New
alert rule*. Both target an email contact point.

### 1. Threshold breach

The daemon already evaluates thresholds with hysteresis and emits a log
record on every transition. The Loki rule fires on each new event:

```logql
count_over_time({service_name="rpi-sentinel", event_type="threshold_exceeded"}[1m]) > 0
```

- Folder: `rpi-sentinel`
- Group interval: `30s`
- For: `0s` (fire immediately — the daemon's hysteresis already filters flapping)
- Summary: `Sensor breach`
- Description: `{{ $labels.sensor_id }} ({{ $labels.metric }}) crossed threshold`

### 2. Silent sensor

Detects daemon crash or stalled reader thread:

```promql
absent_over_time(sensor_reading[2m])
```

- For: `1m` (give the daemon a chance to recover)
- Summary: `Sensor silent`
- Description: `No readings received from {{ $labels.sensor_id }} for 2 minutes`

## Smoke checklist after Phase 2 deployment

After enabling OTLP and importing the dashboard, verify the chain end to
end:

1. Daemon logs show `[otlp] Exporter initialized`.
2. In Grafana → *Explore → Mimir*, query `sensor_reading` — series appear
   within ~1 minute of daemon start.
3. In Grafana → *Explore → Loki*, query `{service_name="rpi-sentinel"}` —
   no logs unless thresholds are crossed.
4. Force a breach (e.g. lower `threshold_warn` in config to a value below
   the current reading, restart the daemon) — a `threshold_exceeded` log
   record appears within 2-3 seconds.
5. The breach alert fires; an email arrives within ~1-2 minutes.
6. Stop the daemon → after 2-3 minutes, the silent-sensor alert fires.

## Troubleshooting

- **No metrics in Grafana**: check the daemon log for HTTP errors after
  `[otlp] Exporter initialized`. The most common causes are an expired
  token (regenerate in Grafana) or a wrong endpoint sub-region.
- **Metrics arrive but no threshold lines**: the dashboard expects
  `sensor_threshold_warn` and `sensor_threshold_crit`. Confirm in *Explore*
  that those series are present. If absent, the daemon is older than the
  `feat(otel)` change that introduced them; rebuild and redeploy.
- **Dashboard repeats nothing**: the `$sensor_id` variable returned no
  values, meaning Mimir hasn't ingested any `sensor_reading` series yet.
  Wait one export interval and refresh.
