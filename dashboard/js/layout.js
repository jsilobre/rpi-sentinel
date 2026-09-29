// ── Free-form layout (drag to move, resize from corner) ─────────────────────────

function loadLayout() {
  try { return JSON.parse(localStorage.getItem(LAYOUT_KEY)) || {}; }
  catch { return {}; }
}

// Whether the narrow-screen stacked layout is active (see STACKED_LAYOUT_QUERY).
function isStackedLayout() {
  return typeof window.matchMedia === 'function' && window.matchMedia(STACKED_LAYOUT_QUERY).matches;
}

function saveLayout() {
  // Stacked geometry comes from CSS; saving it would overwrite the free-form
  // layout arranged on a wider window.
  if (isStackedLayout()) return;
  const layout = loadLayout();
  document.querySelectorAll('.card[data-sensor-id]').forEach(c => {
    layout[c.dataset.sensorId] = {
      left:   c.offsetLeft,
      top:    c.offsetTop,
      width:  c.offsetWidth,
      height: c.offsetHeight,
    };
  });
  try { localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout)); } catch {}
}

// How many default-width cards fit across `containerWidth`.
function gridColumns(containerWidth) {
  return Math.max(1, Math.floor((containerWidth + LAYOUT_GAP) / (DEFAULT_CARD_W + LAYOUT_GAP)));
}

// Place a card at its default size in grid slot `index` of a `cols`-wide grid.
function positionCardInGrid(card, index, cols) {
  const col = index % cols;
  const row = Math.floor(index / cols);
  card.style.left   = (col * (DEFAULT_CARD_W + LAYOUT_GAP)) + 'px';
  card.style.top    = (row * (DEFAULT_CARD_H + LAYOUT_GAP)) + 'px';
  card.style.width  = DEFAULT_CARD_W + 'px';
  card.style.height = DEFAULT_CARD_H + 'px';
}

// The card a new sensor's card is inserted before (null = append), so the DOM
// order stays alphabetical: that is the order the stacked layout shows.
function nextCardInOrder(cards, sensorId) {
  return cards.find(c => c.dataset.sensorId.localeCompare(sensorId) > 0) || null;
}

function placeCard(card, sensorId, existingCount) {
  const saved = loadLayout()[sensorId];
  if (saved) {
    const grid = card.parentElement;
    // While stacked, CSS ignores these values: apply them unclamped so the
    // arrangement is intact when the window widens again.
    const containerWidth = isStackedLayout()
      ? Infinity
      : (grid && grid.clientWidth) || window.innerWidth || 1200;
    const width  = Math.max(280, Math.min(saved.width  ?? DEFAULT_CARD_W, containerWidth));
    const height = Math.max(200, saved.height ?? DEFAULT_CARD_H);
    const left   = Math.max(0, Math.min(saved.left ?? 0, Math.max(0, containerWidth - width)));
    const top    = Math.max(0, saved.top ?? 0);
    card.style.left   = left   + 'px';
    card.style.top    = top    + 'px';
    card.style.width  = width  + 'px';
    card.style.height = height + 'px';
    return;
  }
  const grid = card.parentElement;
  const containerWidth = grid.clientWidth || 1200;
  positionCardInGrid(card, existingCount, gridColumns(containerWidth));
}

function updateGridHeight() {
  const grid = document.getElementById('sensors-grid');
  let maxBottom = 200;
  grid.querySelectorAll('.card[data-sensor-id]').forEach(c => {
    const bottom = c.offsetTop + c.offsetHeight;
    if (bottom > maxBottom) maxBottom = bottom;
  });
  grid.style.minHeight = (maxBottom + LAYOUT_GAP) + 'px';
}

function makeDraggable(card) {
  const head = card.querySelector('.sensor-head');
  if (!head) return;
  head.addEventListener('pointerdown', e => {
    if (isStackedLayout()) return;
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    if (e.target.closest('button, input, select, a, label')) return;
    const grid     = card.parentElement;
    const gridRect = grid.getBoundingClientRect();
    const cardRect = card.getBoundingClientRect();
    const offsetX  = e.clientX - cardRect.left;
    const offsetY  = e.clientY - cardRect.top;
    const prevUserSelect = document.body.style.userSelect;
    const pointerId = e.pointerId;
    document.body.style.userSelect = 'none';
    try { head.setPointerCapture(pointerId); } catch {}

    function onMove(ev) {
      if (ev.pointerId !== pointerId) return;
      const x = ev.clientX - gridRect.left - offsetX;
      const y = ev.clientY - gridRect.top  - offsetY;
      card.style.left = Math.max(0, x) + 'px';
      card.style.top  = Math.max(0, y) + 'px';
      updateGridHeight();
    }
    function onUp(ev) {
      if (ev.pointerId !== pointerId) return;
      head.removeEventListener('pointermove', onMove);
      head.removeEventListener('pointerup', onUp);
      head.removeEventListener('pointercancel', onUp);
      try { head.releasePointerCapture(pointerId); } catch {}
      document.body.style.userSelect = prevUserSelect;
      saveLayout();
    }
    head.addEventListener('pointermove', onMove);
    head.addEventListener('pointerup', onUp);
    head.addEventListener('pointercancel', onUp);
    e.preventDefault();
  });
}

let _layoutSaveTimer = null;
const cardResizeObserver = new ResizeObserver(() => {
  clearTimeout(_layoutSaveTimer);
  _layoutSaveTimer = setTimeout(() => { saveLayout(); updateGridHeight(); }, 150);
});

// ── Organize sensors alphabetically ─────────────────────────────────────────────
function organizeSensors() {
  const grid  = document.getElementById('sensors-grid');
  const cards = Array.from(grid.querySelectorAll('.card[data-sensor-id]'));
  if (!cards.length) return;

  cards.sort((a, b) => a.dataset.sensorId.localeCompare(b.dataset.sensorId));

  const cols = gridColumns(grid.clientWidth || 1200);
  cards.forEach((card, i) => positionCardInGrid(card, i, cols));

  saveLayout();
  updateGridHeight();
}
