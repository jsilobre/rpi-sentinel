// ── Small shared helpers ────────────────────────────────────────────────────────

function domId(sensorId) { return sensorId.replace(/[^a-zA-Z0-9_-]/g, '_'); }

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c =>
    ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[c]));
}

// Label granularity comes from the window's `labels` field (see WINDOWS in
// state.js). 'live' has no entry and falls through to HH:MM:SS.
function fmt(isoOrMs) {
  const d = new Date(isoOrMs);
  const w = WINDOWS[currentWindow];
  switch (w && w.labels) {
    case 'date-year':
      return d.toLocaleDateString('en', {year:'numeric', month:'short', day:'numeric'});
    case 'date':
      return d.toLocaleDateString('en', {month:'short', day:'numeric'});
    case 'datetime':
      return d.toLocaleDateString('en', {month:'short', day:'numeric'}) + ' ' +
             d.toLocaleTimeString('en', {hour:'2-digit', minute:'2-digit', hour12: false});
    default:
      return d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit', hour12: false});
  }
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// A UUID when available, otherwise a good-enough random id.
function newRequestId() {
  return (crypto.randomUUID && crypto.randomUUID())
    || (Date.now().toString(36) + Math.random().toString(36).slice(2));
}

// Hide a sensor card's "reset zoom" button (no-op if the card isn't rendered).
function hideResetZoom(sensorId) {
  const btn = document.getElementById('reset-zoom-' + domId(sensorId));
  if (btn) btn.style.display = 'none';
}

// Alert level for a reading, derived client-side so the card status is right
// even when the dashboard was opened after the threshold was crossed (alert
// events are one-shot and not retained on the broker). Mirrors the daemon's
// hysteresis: once raised, a level only clears when the value drops below
// threshold - hysteresis. `thr` is { warn, crit } or undefined.
function levelFor(value, thr, prev, hysteresis = 0) {
  if (!thr || !Number.isFinite(value)) return 'ok';
  const h = Number.isFinite(hysteresis) ? hysteresis : 0;
  if (value >= thr.crit || (prev === 'crit' && value >= thr.crit - h)) return 'crit';
  if (value >= thr.warn || ((prev === 'warn' || prev === 'crit') && value >= thr.warn - h)) return 'warn';
  return 'ok';
}
