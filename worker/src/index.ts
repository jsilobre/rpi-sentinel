export interface Env {
  DB: D1Database;
  API_KEY: string;
}

interface IngestRow {
  sensor_id: string;
  metric:    string;
  value:     number;
  ts:        number;
}

interface HistoryPoint {
  ts:    number;
  value: number;
}

interface AggregatePoint {
  ts:  number;
  avg: number;
  min: number;
  max: number;
}

const HOUR_MS = 3_600_000;

// Cron self-heals up to this many trailing hours on every tick, so a missed
// run (or late-arriving readings for the current partial hour) is corrected.
const ROLLUP_LOOKBACK_MS = 6 * HOUR_MS;

const CORS: HeadersInit = {
  'Access-Control-Allow-Origin':  '*',
  'Access-Control-Allow-Methods': 'GET, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json', ...CORS },
  });
}

function err(msg: string, status: number): Response {
  return new Response(msg, { status });
}

// ── POST /ingest ──────────────────────────────────────────────────────────────
// Accepts a single IngestRow object or an array of up to 500 rows.
// Auth: Authorization: Bearer <API_KEY>
async function handleIngest(request: Request, env: Env): Promise<Response> {
  const auth = request.headers.get('Authorization') ?? '';
  if (auth !== `Bearer ${env.API_KEY}`) return err('Unauthorized', 401);

  let body: unknown;
  try { body = await request.json(); }
  catch { return err('Invalid JSON', 400); }

  const rows: IngestRow[] = Array.isArray(body) ? body : [body as IngestRow];

  if (rows.length === 0)          return err('Empty batch', 400);
  if (rows.length > 500)          return err('Batch too large (max 500)', 400);

  for (const r of rows) {
    if (typeof r.sensor_id !== 'string' || !r.sensor_id) return err('Missing sensor_id', 400);
    if (typeof r.metric    !== 'string' || !r.metric)    return err('Missing metric', 400);
    if (typeof r.value     !== 'number')                  return err('Missing value', 400);
    if (typeof r.ts        !== 'number')                  return err('Missing ts', 400);
  }

  const stmts = rows.map(r =>
    env.DB.prepare(
      'INSERT INTO readings (sensor_id, metric, value, ts) VALUES (?, ?, ?, ?)'
    ).bind(r.sensor_id, r.metric, r.value, r.ts)
  );
  await env.DB.batch(stmts);

  return json({ ok: true, count: rows.length });
}

// ── GET /history ──────────────────────────────────────────────────────────────
// Query params:
//   sensor_id  (required)
//   since_ts   epoch ms — lower bound (inclusive)
//   until_ts   epoch ms — upper bound (inclusive); used by the custom range picker
//   limit      1–2000, default 500
//   bucket_ms  optional; when >= 3600000 (1h) the response is down-sampled to
//              one point per bucket, served from the readings_hourly rollup
//              table. Used by the 1mo / 6mo / 1y windows (and large custom
//              ranges) so a year of data is ~365 points, not millions.
//
// Raw response (no bucket_ms):
//   { sensor_id, points: [{ts, value}], truncated }
// Aggregated response (bucket_ms >= 1h):
//   { sensor_id, points: [{ts, avg, min, max}], aggregated: true }
async function handleHistory(request: Request, env: Env): Promise<Response> {
  const url      = new URL(request.url);
  const sensorId = url.searchParams.get('sensor_id');
  if (!sensorId) return new Response('Missing sensor_id', { status: 400, headers: CORS });

  const sinceTs  = parseInt(url.searchParams.get('since_ts')  ?? '0',    10);
  const untilTs  = parseInt(url.searchParams.get('until_ts')  ?? '0',    10);
  const bucketMs = parseInt(url.searchParams.get('bucket_ms') ?? '0',    10);

  if (bucketMs >= HOUR_MS) {
    return handleAggregatedHistory(env, sensorId, sinceTs, untilTs, bucketMs);
  }

  const rawLimit = parseInt(url.searchParams.get('limit') ?? '500', 10);
  const limit    = Math.min(Math.max(rawLimit, 1), 2000);

  // Build the window filter dynamically to keep index usage optimal.
  const conditions: string[] = ['sensor_id = ?'];
  const filterParams: (string | number)[] = [sensorId];

  if (sinceTs > 0) { conditions.push('ts >= ?'); filterParams.push(sinceTs); }
  if (untilTs > 0) { conditions.push('ts <= ?'); filterParams.push(untilTs); }
  const where = conditions.join(' AND ');

  // A naive `ORDER BY ts DESC LIMIT n` returns only the most recent n points,
  // which for a 24h/7d window (tens of thousands of readings) collapses to just
  // the last few minutes — making those filters look broken. Instead, downsample
  // uniformly across the whole window so the curve spans the full range, matching
  // HistoryStore::since() on the MQTT path. NTILE splits the windowed rows into
  // `limit` even buckets; taking the first row of each bucket yields at most
  // `limit` points that cover the entire window (first sample preserved).
  const sql = `
    WITH windowed AS (
      SELECT ts, value,
             NTILE(?) OVER (ORDER BY ts ASC) AS bucket,
             COUNT(*)  OVER ()               AS total
      FROM readings
      WHERE ${where}
    ),
    bucketed AS (
      SELECT ts, value, total,
             ROW_NUMBER() OVER (PARTITION BY bucket ORDER BY ts ASC) AS rb
      FROM windowed
    )
    SELECT ts, value, total FROM bucketed WHERE rb = 1 ORDER BY ts ASC`;

  const result = await env.DB
    .prepare(sql)
    .bind(limit, ...filterParams)
    .all<HistoryPoint & { total: number }>();

  const total     = result.results.length ? result.results[0].total : 0;
  const truncated = total > limit;
  const points    = result.results.map(({ ts, value }) => ({ ts, value }));

  return json({ sensor_id: sensorId, points, truncated });
}

// Down-sampled history served from the hourly rollup. Hourly rows are
// re-bucketed into the requested bucket (e.g. 6h or 1d) on the fly; the
// per-bucket average is count-weighted across hours so it matches the true
// mean, and min/max preserve the extremes (e.g. threshold breaches).
async function handleAggregatedHistory(
  env: Env, sensorId: string, sinceTs: number, untilTs: number, bucketMs: number,
): Promise<Response> {
  const conditions: string[] = ['sensor_id = ?'];
  const params: (string | number)[] = [sensorId];

  if (sinceTs > 0) { conditions.push('hour_ts >= ?'); params.push(sinceTs); }
  if (untilTs > 0) { conditions.push('hour_ts <= ?'); params.push(untilTs); }

  // (hour_ts / bucketMs) * bucketMs floors each hour to its bucket start.
  const sql =
    `SELECT (hour_ts / ?) * ? AS ts,
            SUM(avg_val * count) / SUM(count) AS avg,
            MIN(min_val) AS min,
            MAX(max_val) AS max
       FROM readings_hourly
      WHERE ${conditions.join(' AND ')}
      GROUP BY ts
      ORDER BY ts ASC`;

  const result = await env.DB
    .prepare(sql)
    .bind(bucketMs, bucketMs, ...params)
    .all<AggregatePoint>();

  return json({ sensor_id: sensorId, points: result.results, aggregated: true });
}

// ── GET /export ─────────────────────────────────────────────────────────────────
// Full CSV dump of the raw readings table. Unlike /history there is NO point cap:
// every matching row is returned. The response is streamed and the rows are paged
// internally (keyset on rowid) so arbitrarily large datasets stay within D1's
// per-query result limit and the Worker's memory budget.
//
// Query params (all optional; default = entire table):
//   sensor_id   restrict to one sensor
//   since_ts    epoch ms — lower bound (inclusive)
//   until_ts    epoch ms — upper bound (inclusive)
//
// Auth: public (CORS), same posture as GET /history.
// Columns: sensor_id,metric,ts,iso_time,value
const EXPORT_PAGE_SIZE = 5000;

// Quote a CSV field per RFC 4180 (wrap in quotes, double any internal quote).
function csvField(s: string): string {
  return `"${s.replace(/"/g, '""')}"`;
}

interface ExportRow {
  rowid:     number;
  sensor_id: string;
  metric:    string;
  value:     number;
  ts:        number;
}

function handleExport(request: Request, env: Env): Response {
  const url      = new URL(request.url);
  const sensorId = url.searchParams.get('sensor_id');
  const sinceTs  = parseInt(url.searchParams.get('since_ts') ?? '0', 10);
  const untilTs  = parseInt(url.searchParams.get('until_ts') ?? '0', 10);

  const conditions: string[] = ['rowid > ?'];
  const baseParams: (string | number)[] = [];
  if (sensorId)    { conditions.push('sensor_id = ?'); baseParams.push(sensorId); }
  if (sinceTs > 0) { conditions.push('ts >= ?');       baseParams.push(sinceTs); }
  if (untilTs > 0) { conditions.push('ts <= ?');       baseParams.push(untilTs); }

  const sql =
    `SELECT rowid, sensor_id, metric, value, ts
       FROM readings
      WHERE ${conditions.join(' AND ')}
      ORDER BY rowid ASC
      LIMIT ${EXPORT_PAGE_SIZE}`;

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      controller.enqueue(encoder.encode('sensor_id,metric,ts,iso_time,value\n'));
      let lastRowId = 0;
      try {
        for (;;) {
          const page = await env.DB
            .prepare(sql)
            .bind(lastRowId, ...baseParams)
            .all<ExportRow>();
          const rows = page.results;
          if (rows.length === 0) break;

          let chunk = '';
          for (const r of rows) {
            const iso = new Date(r.ts).toISOString();
            chunk += `${csvField(r.sensor_id)},${csvField(r.metric)},${r.ts},${iso},${r.value}\n`;
          }
          controller.enqueue(encoder.encode(chunk));

          lastRowId = rows[rows.length - 1].rowid;
          if (rows.length < EXPORT_PAGE_SIZE) break;
        }
        controller.close();
      } catch (e) {
        controller.error(e);
      }
    },
  });

  const date = new Date().toISOString().slice(0, 10);
  return new Response(stream, {
    status: 200,
    headers: {
      'Content-Type':        'text/csv; charset=utf-8',
      'Content-Disposition': `attachment; filename="rpi-sentinel-export-${date}.csv"`,
      ...CORS,
    },
  });
}

// ── Scheduled rollup (cron) ─────────────────────────────────────────────────────
// Re-aggregates the trailing ROLLUP_LOOKBACK_MS of raw readings into one row
// per sensor per hour, upserting so re-runs and late readings self-correct.
async function runRollup(env: Env): Promise<number> {
  const now   = Date.now();
  const since = Math.floor((now - ROLLUP_LOOKBACK_MS) / HOUR_MS) * HOUR_MS;

  const rows = await env.DB.prepare(
    `SELECT sensor_id,
            metric,
            (ts / ?) * ? AS hour_ts,
            AVG(value)   AS avg_val,
            MIN(value)   AS min_val,
            MAX(value)   AS max_val,
            COUNT(*)     AS count
       FROM readings
      WHERE ts >= ?
      GROUP BY sensor_id, metric, hour_ts`
  ).bind(HOUR_MS, HOUR_MS, since).all<{
    sensor_id: string; metric: string; hour_ts: number;
    avg_val: number; min_val: number; max_val: number; count: number;
  }>();

  if (rows.results.length === 0) return 0;

  const stmts = rows.results.map(r =>
    env.DB.prepare(
      `INSERT INTO readings_hourly
         (sensor_id, hour_ts, metric, avg_val, min_val, max_val, count)
       VALUES (?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(sensor_id, hour_ts) DO UPDATE SET
         metric  = excluded.metric,
         avg_val = excluded.avg_val,
         min_val = excluded.min_val,
         max_val = excluded.max_val,
         count   = excluded.count`
    ).bind(r.sensor_id, r.hour_ts, r.metric, r.avg_val, r.min_val, r.max_val, r.count)
  );
  await env.DB.batch(stmts);

  return rows.results.length;
}

// ── Router ────────────────────────────────────────────────────────────────────
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const { method, url } = request;
    const path = new URL(url).pathname;

    if (method === 'OPTIONS') return new Response(null, { status: 204, headers: CORS });
    if (method === 'POST' && path === '/ingest')  return handleIngest(request, env);
    if (method === 'GET'  && path === '/history') return handleHistory(request, env);
    if (method === 'GET'  && path === '/export')  return handleExport(request, env);

    return new Response('Not found', { status: 404 });
  },

  async scheduled(_event: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(runRollup(env));
  },
};
