// Dependency-free tests for the dashboard front-end.
//
// Runs with Node's built-in test runner and `vm` module — no npm install:
//
//   node --test dashboard/tests/
//
// Two kinds of checks:
//   1. Static  — every JS file compiles, the bundle has no duplicate global
//      declarations, and index.html still carries the deploy placeholders.
//   2. Behavioural — the pure helper functions are executed inside a tiny
//      browser-like sandbox (the scripts are classic scripts sharing one
//      global scope, so we concatenate and run them exactly like the page).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import vm from 'node:vm';

const dashDir = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (rel) => readFileSync(join(dashDir, rel), 'utf8');

// Scripts in their <script> load order (mirrors index.html).
const SCRIPTS = [
  'js/state.js', 'js/utils.js', 'js/layout.js', 'js/charts.js',
  'js/history.js', 'js/combined.js', 'js/config-panel.js',
  'js/handlers.js', 'js/main.js',
];

// Stand-in for the inline config block injected at deploy time.
const CONFIG_PRELUDE = `
  const BROKER_WSS='';const MQTT_USER='';const MQTT_PASS='';
  const TOPIC_PREFIX='rpi';const CLOUD_WORKER_URL='';
  const CLOUD_ENABLED=false;
`;

// ── Static checks ────────────────────────────────────────────────────────────────
test('every JS file parses without syntax errors', () => {
  for (const f of SCRIPTS) {
    assert.doesNotThrow(() => new vm.Script(read(f), { filename: f }), `syntax error in ${f}`);
  }
});

test('bundle has no duplicate global declarations', () => {
  // Duplicate top-level const/let/function across classic scripts is an early
  // SyntaxError when they share one scope — compiling the concatenation catches it.
  const bundle = [CONFIG_PRELUDE, ...SCRIPTS.map(read)].join('\n;\n');
  assert.doesNotThrow(() => new vm.Script(bundle, { filename: 'bundle.js' }));
});

test('index.html keeps the deploy-time placeholders', () => {
  const html = read('index.html');
  for (const ph of ['__MQTT_BROKER_WSS__', '__MQTT_USER__', '__MQTT_PASS__', '__CLOUD_WORKER_URL__']) {
    assert.ok(html.includes(ph), `missing placeholder ${ph} (deploy sed would break)`);
  }
});

// ── Behavioural checks on the pure helpers ───────────────────────────────────────
// Load state + the pure-helper modules into a shared sandbox and expose what we test.
function loadHelpers() {
  const store = {};
  const sandbox = {
    console,
    crypto: globalThis.crypto,
    localStorage: {
      getItem: (k) => (k in store ? store[k] : null),
      setItem: (k, v) => { store[k] = String(v); },
      removeItem: (k) => { delete store[k]; },
    },
    // Browser globals touched at load time by these files.
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  const ctx = vm.createContext(sandbox);

  const files = ['js/state.js', 'js/utils.js', 'js/layout.js', 'js/combined.js'];
  const epilogue = `
    globalThis.__T__ = {
      domId, escapeHtml, fmt, newRequestId, statusBadge, snapshotToEvents,
      loadAdminCreds, mqttCredentials, ADMIN_CREDS_KEY,
      unitFor, axisTitle, gridColumns, positionCardInGrid,
      setWindow: (w) => { currentWindow = w; },
      WINDOWS,
    };
  `;
  const src = [CONFIG_PRELUDE, ...files.map(read), epilogue].join('\n;\n');
  vm.runInContext(src, ctx, { filename: 'helpers-bundle.js' });
  return ctx.__T__;
}

const H = loadHelpers();

test('domId sanitises ids for use in element ids', () => {
  assert.equal(H.domId('temp1'), 'temp1');
  assert.equal(H.domId('living room/temp.1'), 'living_room_temp_1');
});

test('escapeHtml neutralises markup', () => {
  assert.equal(H.escapeHtml('<b>"x"&\'y\''), '&lt;b&gt;&quot;x&quot;&amp;&#39;y&#39;');
  assert.equal(H.escapeHtml(42), '42');
});

test('unitFor maps known metrics', () => {
  assert.equal(H.unitFor('temperature'), '°C');
  assert.equal(H.unitFor('humidity'), '%');
  assert.equal(H.unitFor('co2'), 'ppm');
  assert.equal(H.unitFor('tvoc'), 'ppm');
  assert.equal(H.unitFor('unknown'), '');
});

test('axisTitle capitalises and appends the unit', () => {
  assert.equal(H.axisTitle('temperature'), 'Temperature (°C)');
  assert.equal(H.axisTitle('pressure'), 'Pressure');
  assert.equal(H.axisTitle(''), '');
});

test('gridColumns fits default-width cards into the container', () => {
  // DEFAULT_CARD_W = 380, LAYOUT_GAP = 12  ->  slot width 392.
  assert.equal(H.gridColumns(100), 1);   // never below 1
  assert.equal(H.gridColumns(392), 1);
  assert.equal(H.gridColumns(1200), 3);
});

test('positionCardInGrid places a card in the right slot', () => {
  const card = { style: {} };
  H.positionCardInGrid(card, 0, 3);
  assert.deepEqual(card.style, { left: '0px', top: '0px', width: '380px', height: '300px' });
  H.positionCardInGrid(card, 4, 3); // index 4, 3 cols -> col 1, row 1
  assert.equal(card.style.left, '392px');  // 1 * (380 + 12)
  assert.equal(card.style.top, '312px');   // 1 * (300 + 12)
});

test('newRequestId returns distinct non-empty ids', () => {
  const a = H.newRequestId(), b = H.newRequestId();
  assert.equal(typeof a, 'string');
  assert.ok(a.length > 0);
  assert.notEqual(a, b);
});

test('fmt formats by selected window', () => {
  H.setWindow('live');
  assert.match(H.fmt(0), /\d{2}:\d{2}:\d{2}/);   // live: HH:MM:SS
  H.setWindow('365d');
  const yearly = H.fmt(0);                         // long window: includes the year
  assert.match(yearly, /\d{4}/);
});

test('fmt keeps time-of-day on 7d, which is hourly-bucketed but not a long window', () => {
  // Regression guard: 7d is served from the hourly rollup on the Cloudflare
  // path, but its labels must stay "Jan 1 14:00" — hourly points on date-only
  // labels would collapse 24 distinct buckets onto one indistinguishable tick.
  H.setWindow('7d');
  const d7 = H.fmt(0);
  assert.match(d7, /\d{2}:\d{2}/);       // has a time component
  assert.doesNotMatch(d7, /\d{4}/);      // but no year (that is 365d only)

  H.setWindow('30d');
  assert.doesNotMatch(H.fmt(0), /\d{2}:\d{2}/);  // genuinely long windows: date only
});

test('WINDOWS separates rollup bucketing from cloud-only and label concerns', () => {
  const W = H.WINDOWS;

  // 7d reads the hourly rollup on Cloudflare, but still has an MQTT fallback,
  // so it must NOT be marked cloudOnly (that would hide it without the Worker).
  assert.equal(W['7d'].cloudBucketMs, 3_600_000);
  assert.equal(W['7d'].bucketMs, undefined);
  assert.ok(!W['7d'].cloudOnly);

  // The genuinely long windows have no MQTT equivalent and bucket on both paths.
  for (const w of ['30d', '180d', '365d']) {
    assert.ok(W[w].bucketMs >= 3_600_000, `${w} should bucket`);
    assert.equal(W[w].cloudOnly, true, `${w} should be cloud-only`);
    assert.equal(W[w].cloudBucketMs, undefined, `${w} uses bucketMs, not cloudBucketMs`);
  }

  // Short windows stay raw on both paths.
  for (const w of ['1h', '6h', '24h']) {
    assert.equal(W[w].bucketMs, undefined, `${w} should stay raw`);
    assert.equal(W[w].cloudBucketMs, undefined, `${w} should stay raw`);
  }

  // Every window declares its label granularity; fmt() switches on it.
  for (const [name, cfg] of Object.entries(W)) {
    assert.ok(['time', 'datetime', 'date', 'date-year'].includes(cfg.labels),
      `${name} has an unknown labels value: ${cfg.labels}`);
  }
});

test('statusBadge maps the daemon-provided level to the card badge', () => {
  assert.deepEqual({ ...H.statusBadge('ok') },   { text: 'OK',   cls: 'ok' });
  assert.deepEqual({ ...H.statusBadge('warn') }, { text: 'Warn', cls: 'warn' });
  assert.deepEqual({ ...H.statusBadge('crit') }, { text: 'Crit', cls: 'alert' });
  // Older daemons send no level: the caller must leave the badge alone.
  assert.equal(H.statusBadge(undefined), null);
  assert.equal(H.statusBadge('bogus'), null);
});

test('snapshotToEvents keeps the retained alert list, newest first', () => {
  const alerts = [
    { sensor_id: 'sgp30-tvoc', type: 'EXCEEDED', level: 'warn', timestamp: '2026-09-23T09:45:46Z' },
    { sensor_id: 'dht11-temp', type: 'EXCEEDED', level: 'crit', timestamp: '2026-09-23T09:40:00Z' },
    { sensor_id: 'cpu-temp',   type: 'RECOVERED', level: 'warn', timestamp: '2026-09-23T09:30:00Z' },
  ];
  const out = H.snapshotToEvents({ alerts }, 0, 50);
  assert.deepEqual(out.map(e => e.sensor_id), ['sgp30-tvoc', 'dht11-temp', 'cpu-temp']);

  // Truncated to the timeline size.
  assert.equal(H.snapshotToEvents({ alerts }, 0, 2).length, 2);

  // Entries older than a local "Clear Data" are dropped.
  const clearedAt = Date.parse('2026-09-23T09:35:00Z');
  assert.deepEqual(H.snapshotToEvents({ alerts }, clearedAt, 50).map(e => e.sensor_id),
    ['sgp30-tvoc', 'dht11-temp']);
});

test('snapshotToEvents tolerates malformed payloads', () => {
  assert.equal(H.snapshotToEvents({}, 0, 50).length, 0);
  assert.equal(H.snapshotToEvents({ alerts: 'nope' }, 0, 50).length, 0);
  assert.equal(H.snapshotToEvents(null, 0, 50).length, 0);
  assert.equal(H.snapshotToEvents({ alerts: [null, { foo: 1 }] }, 0, 50).length, 0);
});

// Minimal Storage stand-in for the admin-credential helpers.
function fakeStorage(init = {}) {
  const m = { ...init };
  return { getItem: k => (k in m ? m[k] : null) };
}

test('loadAdminCreds returns null unless a complete pair is stored', () => {
  const K = H.ADMIN_CREDS_KEY;
  assert.equal(H.loadAdminCreds([fakeStorage()]), null);
  assert.equal(H.loadAdminCreds([fakeStorage({ [K]: '{not json' })]), null);
  assert.equal(H.loadAdminCreds([fakeStorage({ [K]: '{"username":"a"}' })]), null);
  assert.equal(H.loadAdminCreds([fakeStorage({ [K]: '{"username":"","password":"x"}' })]), null);
  const throwing = { getItem() { throw new Error('SecurityError'); } };
  assert.equal(H.loadAdminCreds([throwing]), null);
});

test('loadAdminCreds prefers the first storage holding valid credentials', () => {
  const K = H.ADMIN_CREDS_KEY;
  const session = fakeStorage({ [K]: '{"username":"s","password":"1"}' });
  const local   = fakeStorage({ [K]: '{"username":"l","password":"2"}' });
  assert.deepEqual({ ...H.loadAdminCreds([session, local]) }, { username: 's', password: '1' });
  assert.deepEqual({ ...H.loadAdminCreds([fakeStorage(), local]) }, { username: 'l', password: '2' });
});

test('mqttCredentials only grants publish in admin mode', () => {
  // Viewer: the public (read-only) credentials, and never publish — HiveMQ
  // disconnects a client that publishes without the right.
  assert.deepEqual({ ...H.mqttCredentials(null, 'viewer', 'vp') },
    { username: 'viewer', password: 'vp', canPublish: false });
  assert.deepEqual({ ...H.mqttCredentials({ username: 'admin', password: 'ap' }, 'viewer', 'vp') },
    { username: 'admin', password: 'ap', canPublish: true });
});
