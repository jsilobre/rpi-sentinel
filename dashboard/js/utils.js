// ── Small shared helpers ────────────────────────────────────────────────────────

function domId(sensorId) { return sensorId.replace(/[^a-zA-Z0-9_-]/g, '_'); }

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c =>
    ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[c]));
}

function fmt(isoOrMs) {
  const d = new Date(isoOrMs);
  const w = WINDOWS[currentWindow];
  if (w && w.bucketMs)  // long windows: date only (with year for 1y)
    return d.toLocaleDateString('en', currentWindow === '365d'
      ? {year:'numeric', month:'short', day:'numeric'}
      : {month:'short', day:'numeric'});
  if (currentWindow === '24h' || currentWindow === '7d')
    return d.toLocaleDateString('en', {month:'short', day:'numeric'}) + ' ' +
           d.toLocaleTimeString('en', {hour:'2-digit', minute:'2-digit', hour12: false});
  return d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit', hour12: false});
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
