# Cloudflare setup — Worker + D1

This guide covers the one-time setup needed to deploy the rpi-sentinel Cloudflare Worker and wire it up to the dashboard and the RPi daemon.

## Architecture

```
RPi daemon (CloudStorageHandler)
  └─ HTTP POST /ingest ──► Cloudflare Worker ──► D1: readings (raw)
                                                  │         ▲
                            cron (hourly) rollup ─┘         │
                                  └────────► D1: readings_hourly (avg/min/max)
                                                        ▲
Dashboard (GitHub Pages) ─── GET /history?... ──────────┘
```

The Worker has two HTTP endpoints plus a scheduled (cron) handler:

| Trigger | Caller | Auth |
|---|---|---|
| `POST /ingest` | RPi daemon | `Authorization: Bearer <API_KEY>` |
| `GET /history` | Dashboard (browser) | None (CORS public) |
| cron `0 * * * *` | Cloudflare scheduler | n/a |

### Time windows & down-sampling

Short windows (`1h`–`7d`) and the custom picker for spans ≤ 7 days return **raw**
points from the `readings` table. The long windows — **1mo / 6mo / 1y** (and custom
ranges wider than 7 days) — would be far too many raw points (a year at a 5 s poll
is ~6.3 M rows per sensor), so they are served **down-sampled** from the
`readings_hourly` rollup table:

| Window | `bucket_ms` | Points | Source |
|---|---|---|---|
| 1mo | 1 h (3 600 000) | ~720 | `readings_hourly` |
| 6mo | 6 h (21 600 000) | ~720 | `readings_hourly` (re-bucketed) |
| 1y | 1 d (86 400 000) | ~365 | `readings_hourly` (re-bucketed) |

Each down-sampled point carries an **average plus a min/max band** so short spikes
(e.g. threshold breaches) are preserved rather than smoothed away. The hourly cron
re-aggregates a trailing 6 h of raw readings on every run, so a missed tick or a
late-arriving reading self-corrects (upsert keyed on `sensor_id, hour_ts`).

These long windows are **Cloudflare-only** — there is no MQTT equivalent; the
buttons stay hidden unless `CLOUD_WORKER_URL` is injected into the dashboard.

---

## Prerequisites

- A Cloudflare account (free tier is sufficient)
- The `jsilobre/rpi-sentinel` GitHub repository connected to Cloudflare Pages/Workers (via the Cloudflare dashboard → "Workers & Pages" → "Connect to Git")

---

## 1. Create the Worker via Cloudflare Git integration

1. Go to **Workers & Pages** → **Create** → **Import a Git repository**
2. Select the `rpi-sentinel` repository
3. Set the **Worker name**: `rpi-sentinel-worker`
4. Under **Advanced settings**:
   - **Root directory**: `/worker`
   - **Build command**: `npm install`
   - **Deploy command**: `npx wrangler deploy`
5. Save and deploy — the first deploy will fail because the D1 database doesn't exist yet. That's expected.

The Worker URL will be `https://rpi-sentinel-worker.<your-account>.workers.dev`.

---

## 2. Create the D1 database

In the Cloudflare dashboard, go to **Storage & Databases** → **D1** → **Create database**.

- **Database name**: `rpi-sentinel-db`

Copy the **Database ID** shown after creation.

### Update `wrangler.toml`

Edit `worker/wrangler.toml` and paste the ID:

```toml
[[d1_databases]]
binding        = "DB"
database_name  = "rpi-sentinel-db"
database_id    = "<paste-id-here>"
```

Commit and push to `main`. The Worker will redeploy automatically.

### Apply the schema

In the Cloudflare dashboard → **D1** → **rpi-sentinel-db** → **Console**, paste the
full contents of [`worker/schema.sql`](../worker/schema.sql) (or run
`npx wrangler d1 execute rpi-sentinel-db --remote --file=worker/schema.sql`). It is
idempotent (`CREATE TABLE IF NOT EXISTS`), so it is safe to re-run on an existing
database to add the `readings_hourly` rollup table:

```sql
CREATE TABLE IF NOT EXISTS readings (
  sensor_id TEXT    NOT NULL,
  ts        INTEGER NOT NULL,
  value     REAL    NOT NULL,
  metric    TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_readings ON readings (sensor_id, ts DESC);

-- Hourly rollup powering the 1mo / 6mo / 1y windows.
CREATE TABLE IF NOT EXISTS readings_hourly (
  sensor_id TEXT    NOT NULL,
  hour_ts   INTEGER NOT NULL,
  metric    TEXT    NOT NULL,
  avg_val   REAL    NOT NULL,
  min_val   REAL    NOT NULL,
  max_val   REAL    NOT NULL,
  count     INTEGER NOT NULL,
  PRIMARY KEY (sensor_id, hour_ts)
);
```

> **Cron trigger**: the hourly rollup is declared in `worker/wrangler.toml`
> (`[triggers] crons = ["0 * * * *"]`) and is registered automatically on the next
> `wrangler deploy`. No dashboard configuration is needed. The rollup table stays
> empty (and the long windows show nothing) until the first cron tick fires after a
> deploy that includes the trigger.

---

## 3. Set the API key (runtime secret)

**Important**: the API key must be set as a **runtime** secret, not a build-time variable.
These are two different sections in Cloudflare's UI and are easy to confuse.

1. Go to **Workers & Pages** → **rpi-sentinel-worker** → **Settings**
2. Scroll to **Variables and Secrets** (the first section at the top — *not* the "Build" subsection)
3. Click **Add** → type `API_KEY` → paste a strong random value (e.g. output of `openssl rand -hex 32`)
4. Mark as **Secret** → **Save**

Keep this value — you'll need it on the RPi side as `CLOUD_API_KEY`.

### Trigger a redeploy to pick up the secret

New runtime secrets are only visible to the Worker after a redeploy. Trigger one with an empty commit:

```bash
git commit --allow-empty -m "chore: trigger worker redeploy"
git push origin main
```

---

## 4. Add the Worker URL to GitHub Secrets

The dashboard's deploy workflow injects the Worker URL at build time.

1. Go to the GitHub repository → **Settings** → **Secrets and variables** → **Actions**
2. Click **New repository secret**
   - **Name**: `CLOUD_WORKER_URL`
   - **Value**: `https://rpi-sentinel-worker.<your-account>.workers.dev`

---

## 5. Configure the RPi daemon

### `config.json`

Add (or update) the `cloud_storage` section:

```json
"cloud_storage": {
  "enabled": true,
  "endpoint": "https://rpi-sentinel-worker.<your-account>.workers.dev",
  "api_key_env": "CLOUD_API_KEY"
}
```

`api_key_env` names the environment variable the daemon reads at startup.
Do not put the key literal in `config.json`.

### Environment variable

Set `CLOUD_API_KEY` in the shell that runs the daemon:

```bash
export CLOUD_API_KEY="<same-value-you-set-in-Cloudflare>"
./rpi-sentinel
```

For a persistent setup (systemd service), add to the service unit:

```ini
[Service]
Environment=CLOUD_API_KEY=<value>
```

### Build requirement

The daemon must be compiled with `ENABLE_CLOUD_STORAGE=ON`, which is auto-detected when `libcurl` is installed:

```bash
sudo apt-get install -y libcurl4-openssl-dev
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Confirm it's active in the CMake output:

```
-- Cloud storage: ON (libcurl 8.x.x)
```

---

## 6. Deploy the dashboard

Push any change to `main` that touches `dashboard/` to trigger `deploy-dashboard.yml`, or re-run the workflow manually from GitHub Actions → **Deploy Dashboard** → **Run workflow**.

The workflow replaces the `__CLOUD_WORKER_URL__` placeholder in `dashboard/index.html` with the `CLOUD_WORKER_URL` secret. Once deployed, the dashboard will show the custom time range picker and query the Worker for historical data.

---

## 7. End-to-end verification

### Test POST /ingest

```bash
curl -s -X POST https://rpi-sentinel-worker.<your-account>.workers.dev/ingest \
  -H "Authorization: Bearer <API_KEY>" \
  -H "Content-Type: application/json" \
  -d '[{"sensor_id":"cpu-temp","metric":"temperature","value":55.1,"ts":1716825600000}]'
# → {"ok":true,"count":1}
```

### Test GET /history

```bash
curl -s "https://rpi-sentinel-worker.<your-account>.workers.dev/history?sensor_id=cpu-temp"
# → {"sensor_id":"cpu-temp","points":[{"ts":1716825600000,"value":55.1}],"truncated":false}
```

### Verify daemon is posting

Start the daemon. Expected log output on success:

```
[cloud] Handler initialized (endpoint: https://rpi-sentinel-worker....)
```

Errors (e.g. 401) appear as:

```
[cloud] POST failed: HTTP 401
```

### Verify dashboard is using the Worker

Open the dashboard → DevTools → **Network** tab → filter on `history`.
Requests to `https://rpi-sentinel-worker.*.workers.dev/history?...` should appear when changing the time window.

---

## 8. Operations

### Inspect stored readings

In the Cloudflare dashboard → **D1** → **rpi-sentinel-db** → **Console**:

```sql
SELECT sensor_id, COUNT(*), MAX(ts) FROM readings GROUP BY sensor_id;
```

### Monitor Worker logs

Go to **Workers & Pages** → **rpi-sentinel-worker** → **Logs** (real-time tail) or **Metrics** for aggregated stats.

### Query a time range manually

Raw points:

```bash
curl -s "https://rpi-sentinel-worker.<your-account>.workers.dev/history\
?sensor_id=cpu-temp&since_ts=1716739200000&until_ts=1716825600000&limit=2000"
# → {"sensor_id":"cpu-temp","points":[{"ts":...,"value":...}],"truncated":false}
```

Down-sampled (banded) — add `bucket_ms` (≥ 3600000 = 1h); served from the rollup:

```bash
# 1-day buckets over the last year (~365 points), from readings_hourly
curl -s "https://rpi-sentinel-worker.<your-account>.workers.dev/history\
?sensor_id=cpu-temp&since_ts=$(( $(date +%s%3N) - 31536000000 ))&bucket_ms=86400000"
# → {"sensor_id":"cpu-temp","points":[{"ts":...,"avg":...,"min":...,"max":...}],"aggregated":true}
```

### Inspect / force the rollup

```sql
-- How far the rollup has caught up, per sensor:
SELECT sensor_id, COUNT(*), datetime(MAX(hour_ts)/1000,'unixepoch') FROM readings_hourly GROUP BY sensor_id;
```

The cron fires hourly; to backfill `readings_hourly` from existing raw data
immediately (e.g. right after first deploy), run **Trigger Scheduled Event** under
**Workers & Pages** → **rpi-sentinel-worker** → **Settings** → **Trigger Events**,
or `npx wrangler tail` and wait for the next `0 * * * *` tick. Each run only covers a
trailing 6 h window — to backfill a long history at once, run the rollup query from
`runRollup()` in the D1 Console with a wider lower bound.

### Rotate the API key

1. Generate a new key: `openssl rand -hex 32`
2. Update the runtime secret in Cloudflare → **Settings** → **Variables and Secrets**
3. Trigger a redeploy (empty commit or redeploy button)
4. Update `CLOUD_API_KEY` on the RPi and restart the daemon

---

## Failure modes

| Scenario | Behaviour |
|---|---|
| `cloud_storage.enabled = false` | Handler not instantiated; no HTTP traffic |
| `CLOUD_API_KEY` unset | Daemon logs error at startup and continues without cloud storage |
| Network outage on RPi | POST fails silently; items are dropped after the 1 000-item queue cap |
| Worker returns non-200 | Logged and dropped; no retry |
| `CLOUD_WORKER_URL` secret not set in GitHub | Placeholder stays in HTML; `CLOUD_ENABLED = false`; dashboard falls back to MQTT for all history |
| API key mismatch | Worker returns 401; daemon logs `[cloud] POST failed: HTTP 401` |
| Secret set in build-time section (not runtime) | Worker doesn't see it → 401 on every request; fix by moving the secret to the runtime "Variables and Secrets" section and redeploying |
