// ── Shared application state & constants ────────────────────────────────────────
// Loaded as a classic script: every top-level declaration here lives in the
// shared global scope and is visible to the other dashboard scripts. This file
// must be loaded *after* the inline config block (which defines BROKER_WSS,
// CLOUD_WORKER_URL, CLOUD_ENABLED, …) and *before* the feature scripts.

const MAX_HISTORY = 120;
const MAX_EVENTS  = 50;

// Time-window definitions. A window with `bucketMs` is served as a server-side
// down-sampled, min/max-banded view from Cloudflare only — used for the long
// windows where raw points would be far too many for MQTT and the browser.
const WINDOWS = {
  '1h':   { ms: 3_600_000 },
  '6h':   { ms: 21_600_000 },
  '24h':  { ms: 86_400_000 },
  '7d':   { ms: 604_800_000 },
  '30d':  { ms: 2_592_000_000,  bucketMs: 3_600_000 },   // 1h buckets → ~720 pts
  '180d': { ms: 15_552_000_000, bucketMs: 21_600_000 },  // 6h buckets → ~720 pts
  '365d': { ms: 31_536_000_000, bucketMs: 86_400_000 },  // 1d buckets → ~365 pts
};

const charts               = {};
const history              = {};
const events               = [];
const sensorThresholds     = {};
const knownSensorIds       = new Set();
let   lastRpiStatus        = null;
let   firstConnect         = true;
const pendingHydrations    = {};  // request_id → sensorId (initial load)
const pendingHydrationSet  = new Set(); // sensorIds with an in-flight initial hydration
const pendingWindowHydrations = {}; // request_id → sensorId (window switch)
const hydratedSensors      = new Set();
let   clearedAt            = 0;   // epoch-ms; readings older than this are dropped
const pendingSaves         = {};  // sensorId → { warn, crit, fb, sid, timer }
const chartTimestamps      = {};  // sensorId → array of epoch-ms mirroring chart data (historical modes)
const WINDOW_KEY           = 'rpi-sentinel-window';
let   currentWindow        = localStorage.getItem(WINDOW_KEY) || 'live';
// A persisted long (Cloudflare-only) window is meaningless without the Worker.
if (!CLOUD_ENABLED && WINDOWS[currentWindow] && WINDOWS[currentWindow].bucketMs)
  currentWindow = 'live';

const palette = [
  { border: '#0ea5e9', bg: 'rgba(14,165,233,0.10)' },
  { border: '#8b5cf6', bg: 'rgba(139,92,246,0.10)' },
  { border: '#10b981', bg: 'rgba(16,185,129,0.10)' },
  { border: '#f97316', bg: 'rgba(249,115,22,0.10)' },
  { border: '#ec4899', bg: 'rgba(236,72,153,0.10)' },
];

// ── Combined (all-sensors) view state ──────────────────────────────────────────
const sensorMetric   = {};   // sensorId → metric string (drives Y-axis grouping)
const sensorColor    = {};   // sensorId → palette entry (matches the card colour)
let   combinedChart  = null;
const VIEWMODE_KEY        = 'rpi-sentinel-viewmode';
const COMBINED_HIDDEN_KEY = 'rpi-sentinel-combined-hidden';
let   viewMode       = localStorage.getItem(VIEWMODE_KEY) || 'multi';
function loadCombinedHidden() {
  try { return JSON.parse(localStorage.getItem(COMBINED_HIDDEN_KEY)) || []; }
  catch { return []; }
}
const combinedHidden = new Set(loadCombinedHidden());

// ── Free-form layout constants ─────────────────────────────────────────────────
const LAYOUT_KEY      = 'rpi-sentinel-layout';
const DEFAULT_CARD_W  = 380;
const DEFAULT_CARD_H  = 300;
const LAYOUT_GAP      = 12;
