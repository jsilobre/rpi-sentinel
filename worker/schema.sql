CREATE TABLE IF NOT EXISTS readings (
  sensor_id TEXT    NOT NULL,
  ts        INTEGER NOT NULL,
  value     REAL    NOT NULL,
  metric    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_readings ON readings (sensor_id, ts DESC);

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
