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
      domId, escapeHtml, fmt, newRequestId,
      unitFor, axisTitle, gridColumns, positionCardInGrid,
      setWindow: (w) => { currentWindow = w; },
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
