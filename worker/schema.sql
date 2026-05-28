CREATE TABLE IF NOT EXISTS readings (
  sensor_id TEXT    NOT NULL,
  ts        INTEGER NOT NULL,
  value     REAL    NOT NULL,
  metric    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_readings ON readings (sensor_id, ts DESC);
