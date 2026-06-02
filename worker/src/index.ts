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
//
// Response shape matches the existing MQTT history-on-demand protocol so that
// applyWindowHydration() in the dashboard works without modification:
//   { sensor_id, points: [{ts, value}], truncated }
async function handleHistory(request: Request, env: Env): Promise<Response> {
  const url      = new URL(request.url);
  const sensorId = url.searchParams.get('sensor_id');
  if (!sensorId) return new Response('Missing sensor_id', { status: 400, headers: CORS });

  const sinceTs  = parseInt(url.searchParams.get('since_ts')  ?? '0',    10);
  const untilTs  = parseInt(url.searchParams.get('until_ts')  ?? '0',    10);
  const rawLimit = parseInt(url.searchParams.get('limit')     ?? '500',  10);
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

// ── Router ────────────────────────────────────────────────────────────────────
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const { method, url } = request;
    const path = new URL(url).pathname;

    if (method === 'OPTIONS') return new Response(null, { status: 204, headers: CORS });
    if (method === 'POST' && path === '/ingest')  return handleIngest(request, env);
    if (method === 'GET'  && path === '/history') return handleHistory(request, env);

    return new Response('Not found', { status: 404 });
  },
};
