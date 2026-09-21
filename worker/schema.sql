CREATE TABLE IF NOT EXISTS readings (
  sensor_id TEXT    NOT NULL,
  ts        INTEGER NOT NULL,
  value     REAL    NOT NULL,
  metric    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_readings ON readings (sensor_id, ts DESC);

-- Serves the hourly rollup's `WHERE ts >= ?` scan. Without it, SQLite cannot
-- seek on ts (idx_readings leads with sensor_id), so the cron degrades to
-- `SCAN readings USING INDEX idx_readings` — a full pass over every row in the
-- table, 24 times a day. That alone exhausts D1's free-tier 5M rows-read/day
-- budget once the table passes ~210k rows.
--
-- The column list makes it a COVERING index for that query (verified with
-- EXPLAIN QUERY PLAN: `SEARCH readings USING COVERING INDEX idx_readings_ts
-- (ts>?)`), so the rollup never touches the table itself.
--
-- Trade-off: a second index means one more row written per INSERT, and it
-- roughly doubles storage for the readings table. See docs/cloudflare-setup.md.
CREATE INDEX IF NOT EXISTS idx_readings_ts
  ON readings (ts, sensor_id, metric, value);

-- Hourly rollup table for long time windows (1mo / 6mo / 1y).
-- Populated by the Worker's scheduled (cron) handler; one row per
-- sensor per wall-clock hour. hour_ts is the epoch-ms floored to the
-- start of the hour. Querying these pre-aggregated rows keeps the
-- D1 rows-read cost tiny (<= ~8760 rows for a 1-year window) instead
-- of scanning millions of raw readings.
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
