# Persistence — sensor history

## 1. Why

The cloud dashboard (`dashboard/index.html`, hosted on GitHub Pages) is a pure
browser-side application. Without persistence, refreshing the page wipes
the history and the user has to wait for new readings to redraw the charts.

rpi-sentinel provides two complementary persistence mechanisms:

| Layer | Storage | Retention | Dashboard path |
|---|---|---|---|
| Local SQLite (`HistoryStore`) | `data/history.db` on the Pi | 7 days (configurable) | MQTT history-on-demand (Pi must be online) |
| Cloud D1 (`CloudStorageHandler`) | Cloudflare D1 (edge SQLite) | Unlimited | HTTP GET /history (Pi-independent) |

Both run simultaneously. The dashboard uses MQTT for live data and for short-term history when the Pi is online, and falls back to (or prefers) the Cloudflare Worker for longer historical windows.

---

## 2. Storage

### Schema

```sql
CREATE TABLE IF NOT EXISTS readings(
    sensor_id TEXT    NOT NULL,
    ts        INTEGER NOT NULL,   -- epoch milliseconds
    value     REAL    NOT NULL,
    metric    TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_readings_sensor_ts
    ON readings(sensor_id, ts DESC);
PRAGMA user_version = 1;
```

A single table keeps queries simple, lets rotation operate centrally, and
trivially supports a dynamic set of sensors. JSON-blob storage was rejected
to keep `since(ts)` queries indexable.

### PRAGMAs

Set at open time, tuned for SD-card friendliness:

| PRAGMA | Value | Why |
|---|---|---|
| `journal_mode` | `WAL` | Writers and readers don't block each other |
| `synchronous` | `NORMAL` | Drops the per-commit `fsync`; survives crashes (data safe within WAL checkpoint) |
| `busy_timeout` | `2000` | Avoids spurious `SQLITE_BUSY` while a checkpoint runs |
| `temp_store` | `MEMORY` | Sort/group temp tables stay in RAM |

### File location

Default: `data/history.db` (relative to the daemon's working directory).
The directory is created automatically. Override via `config.json`:

```json
"history": {
    "enabled": true,
    "db_path": "data/history.db",
    "retention_days": 7,
    "max_points_per_sensor": 50000
}
```

### Rotation

`HistoryStore::rotate()` runs two `DELETE` statements:

1. By age — `DELETE FROM readings WHERE ts < now − retention_days`.
2. By count — for each sensor, keep only the latest
   `max_points_per_sensor` rows.

Rotation is amortised: it triggers automatically every 200 inserts (no
`VACUUM` on the hot path). At a 5 s polling interval that's roughly every
17 minutes per sensor, well within the polling cadence.

### Threading

Inserts are synchronous and protected by an internal `std::mutex`. SQLite's
`FULLMUTEX` open flag plus our mutex make concurrent calls safe; with WAL +
`synchronous=NORMAL` a single insert costs ~50–200 µs on a Pi 4 SD card —
two orders of magnitude below the default 5 s poll cadence, so the
event-handler latency budget stays comfortable. If this ever becomes a
bottleneck, a queue + writer thread can be slipped in entirely inside
`SqliteHistoryHandler` without touching callers.

---

## 3. MQTT history-on-demand

The cloud dashboard requests history per sensor as soon as it creates a chart
card. The daemon's existing `MqttPublisher` is extended with a request/response
handler — no new connection, no new authentication path.

### Topics

| Topic | Direction | Retain | QoS |
|---|---|---|---|
| `rpi/history/req` | dashboard → daemon | `false` | 1 |
| `rpi/history/resp/{request_id}` | daemon → dashboard | `false` | 1 |

### Request payload

```json
{
  "request_id": "8c6c9a8e-…",
  "sensor_id":  "bme280-temp",
  "since_ts":   1714000000000,    // optional, epoch ms
  "limit":      120                // optional, default 240, hard-capped server-side
}
```

If `since_ts` is omitted, the server returns the most recent `limit` points.

### Response payload

```json
{
  "request_id": "8c6c9a8e-…",
  "sensor_id":  "bme280-temp",
  "metric":     "temperature",
  "points": [
    {"ts": 1714000010000, "value": 22.4},
    {"ts": 1714000020000, "value": 22.6}
  ],
  "truncated": false
}
```

Points are returned in ascending chronological order. `truncated=true`
indicates the result hit the server-side cap (currently **500** points,
hard-clamped) — the dashboard can then issue a paginated follow-up using
`since_ts` if needed.

A 240-point response is roughly 12 KB of JSON, well under the 256 KB max
payload of HiveMQ Cloud's free tier.

### Hydration sequence (cloud dashboard)

```
page load
   │
   ▼
client.connect()  → subscribe rpi/history/resp/+
   │
   ▼ (config/current arrives, cards are created on first reading)
ensureCard(sensor)
   │
   ▼ requestHydration(sensor)
   │     publish rpi/history/req {request_id, sensor_id, limit}
   │     pendingHydrations[request_id] = sensor
   │
   │ (… meanwhile live readings may arrive and be appended to history[sensor] …)
   │
   ▼ on rpi/history/resp/{request_id}
applyHydration(sensor, points)
   │   filter historical points to ts < first live point's ts
   │   prepend → history[sensor] = [historical, …live]
   │   chart.update('none')
```

A 5 s timeout drops the pending request silently if no response arrives — the
dashboard simply continues with live readings as before. On reconnect, the
client re-issues hydration for any sensors that are not yet hydrated this
session.

### Broker ACL changes

The HiveMQ Cloud user that the dashboard connects with needs two extra ACL
rules in addition to its existing read access:

| Pattern | Permission |
|---|---|
| `rpi/history/req` | publish |
| `rpi/history/resp/+` | subscribe |

Without these, hydration requests will be silently dropped by the broker.

---

## 4. Cloud storage — Cloudflare Worker + D1

`CloudStorageHandler` ships every `Reading` event to a Cloudflare Worker via HTTP POST. The schema mirrors the local SQLite table exactly.

### POST /ingest

```
Authorization: Bearer <API_KEY>
Content-Type: application/json

[{"sensor_id":"cpu-temp","metric":"temperature","value":55.1,"ts":1716825600000}]
```

The daemon batches up to 50 items per request, drained every 2 s from an in-memory queue. If the queue exceeds 1 000 items (network outage), the oldest items are dropped — recent data is preserved.

### GET /history (dashboard → Worker)

```
GET /history?sensor_id=cpu-temp&since_ts=1716739200000&limit=500
```

Response matches the MQTT history-on-demand shape so `applyWindowHydration()` in the dashboard is shared:

```json
{"sensor_id":"cpu-temp","points":[{"ts":1716825600000,"value":55.1}],"truncated":false}
```

The optional `until_ts` parameter supports the custom time range picker in the dashboard.

### Failure modes (cloud)

| Scenario | Behaviour |
|---|---|
| `cloud_storage.enabled = false` | Handler not instantiated; no HTTP traffic. |
| `CLOUD_API_KEY` unset | Construction throws; daemon logs error and continues without cloud storage. |
| Network outage | POST fails silently; items are dropped after queue cap. Live MQTT still works. |
| Worker returns non-200 | Logged and dropped; no retry to avoid unbounded backlog. |
| Dashboard with no `CLOUD_WORKER_URL` secret | `CLOUD_ENABLED = false`; dashboard falls back to MQTT for all history windows. |

See [cloudflare-setup.md](cloudflare-setup.md) for full deployment instructions.

---

## 5. Failure modes

| Scenario | Behaviour |
|---|---|
| `history.enabled = false` | Daemon runs without persistence — same behaviour as pre-feature. |
| `sqlite3_open` fails (permissions, disk full) | Daemon logs the error and continues without persistence; `mqtt_pub->set_history_store` is not called, so history requests come back with empty `points`. |
| RPi offline | Cloud dashboard's history request times out after 5 s; chart simply starts empty and fills with live readings as before. |
| Response hits `truncated=true` | Dashboard renders what it received; user scroll back is naturally limited. |
| Concurrent dashboards | Each request has its own `request_id`, so responses are never aliased. |
| DB file corrupted | Open fails → no persistence; user can delete `data/history.db*` to reset. |

---

## 6. Operations

- Inspect:
  ```bash
  sqlite3 data/history.db "SELECT sensor_id, COUNT(*), MAX(ts) FROM readings GROUP BY sensor_id;"
  ```
- Disk footprint: roughly **30 B/row** in SQLite (vs ~50 B for the JSON
  serialization). At a 5 s poll, 6 sensors and a 7-day retention that's
  ~700 k rows, ~25 MB.
- Backup: copy `data/history.db` plus its sidecars (`-wal`, `-shm`) while the
  daemon is stopped, or use `sqlite3 .backup` while running.
- Disable temporarily: set `"history": {"enabled": false}` in `config.json`
  and restart.
