// ── Combined (all-sensors) graph ────────────────────────────────────────────────
function saveCombinedHidden() {
  try { localStorage.setItem(COMBINED_HIDDEN_KEY, JSON.stringify([...combinedHidden])); } catch {}
}

function unitFor(metric) {
  switch (metric) {
    case 'temperature': return '°C';
    case 'humidity':    return '%';
    case 'co2': case 'eco2': case 'tvoc': return 'ppm';
    default:            return '';
  }
}

function axisTitle(metric) {
  if (!metric) return '';
  const u = unitFor(metric);
  return metric.charAt(0).toUpperCase() + metric.slice(1) + (u ? ` (${u})` : '');
}

// Points for one sensor in the currently-selected window, as {x: epochMs, y}.
function combinedPoints(sensorId) {
  if (currentWindow === 'live') {
    const h = history[sensorId] || [];
    return h.map(p => ({ x: new Date(p.timestamp).getTime(), y: p.value }));
  }
  // Historical / aggregated / custom: mirror the per-sensor chart's series,
  // which is already populated by the existing hydration paths.
  const ts   = chartTimestamps[sensorId] || [];
  const vals = (charts[sensorId] && charts[sensorId].data.datasets[0].data) || [];
  const n    = Math.min(ts.length, vals.length);
  const out  = [];
  for (let i = 0; i < n; i++) out.push({ x: ts[i], y: vals[i] });
  return out;
}

function ensureCombinedChart() {
  if (combinedChart) return combinedChart;
  combinedChart = new Chart(document.getElementById('combined-chart').getContext('2d'), {
    type: 'line',
    data: { datasets: [] },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      parsing: false,
      interaction: { intersect: false, mode: 'nearest', axis: 'x' },
      plugins: {
        legend: {
          display: true, position: 'top',
          labels: { color: cssVar('--muted'), boxWidth: 10, boxHeight: 10, font: { size: 11 } },
          onClick: (e, item, legend) => {
            const ci       = legend.chart;
            const sensorId = ci.data.datasets[item.datasetIndex]._sensorId;
            const visible  = ci.isDatasetVisible(item.datasetIndex);
            ci.setDatasetVisibility(item.datasetIndex, !visible);
            if (visible) combinedHidden.add(sensorId);
            else         combinedHidden.delete(sensorId);
            saveCombinedHidden();
            ci.update();
          }
        },
        tooltip: {
          callbacks: {
            title: items => items.length ? fmt(items[0].parsed.x) : '',
            label: ctx => {
              const ds = ctx.dataset;
              const u  = unitFor(sensorMetric[ds._sensorId] || '');
              return `${ds.label}: ${ctx.parsed.y.toFixed(2)}${u ? ' ' + u : ''}`;
            }
          }
        },
        zoom: {
          zoom: {
            wheel: { enabled: true }, pinch: { enabled: true }, mode: 'x',
            onZoomComplete: () => {
              const btn = document.getElementById('reset-zoom-combined');
              if (btn) btn.style.display = 'inline-block';
            }
          },
          pan: { enabled: true, mode: 'x' }
        }
      },
      scales: {
        x: {
          type: 'linear',
          ticks: { color: cssVar('--muted-2'), maxTicksLimit: 6, maxRotation: 0,
                   font: { size: 10 }, callback: v => fmt(v) },
          grid: { color: cssVar('--grid') }
        }
        // Y axes are (re)built per distinct metric in rebuildCombinedAxes().
      }
    }
  });
  document.getElementById('reset-zoom-combined').addEventListener('click', () => {
    combinedChart.resetZoom();
    document.getElementById('reset-zoom-combined').style.display = 'none';
  });
  return combinedChart;
}

// (Re)build one Y axis per distinct metric; returns a metric→axisId map.
function rebuildCombinedAxes(chart) {
  const metrics = [];
  for (const sid of Object.keys(charts)) {
    const m = sensorMetric[sid] || '';
    if (!metrics.includes(m)) metrics.push(m);
  }
  const scales = { x: chart.options.scales.x };
  scales.x.ticks.color = cssVar('--muted-2');
  scales.x.grid.color  = cssVar('--grid');
  const metricAxis = {};
  metrics.forEach((m, i) => {
    const axisId = i === 0 ? 'y' : 'y' + i;
    metricAxis[m] = axisId;
    scales[axisId] = {
      type: 'linear', position: i === 0 ? 'left' : 'right',
      grid:  { drawOnChartArea: i === 0, color: cssVar('--grid') },
      ticks: { color: cssVar('--muted-2'), callback: v => v.toFixed(1), font: { size: 10 } },
      title: { display: !!m, text: axisTitle(m), color: cssVar('--muted'), font: { size: 10 } },
    };
  });
  chart.options.scales = scales;
  return metricAxis;
}

// Rebuild every dataset from the per-sensor state. Cheap and idempotent; a no-op
// unless the combined view is active.
function syncCombined() {
  if (viewMode !== 'combined') return;
  const chart      = ensureCombinedChart();
  const metricAxis = rebuildCombinedAxes(chart);
  chart.options.plugins.legend.labels.color = cssVar('--muted');

  const sensorIds = Object.keys(charts).sort((a, b) => a.localeCompare(b));
  chart.data.datasets = sensorIds.map(sid => {
    const col = sensorColor[sid] || palette[0];
    return {
      _sensorId: sid, label: sid,
      data: combinedPoints(sid),
      borderColor: col.border, backgroundColor: col.border,
      yAxisID: metricAxis[sensorMetric[sid] || ''] || 'y',
      borderWidth: 2, tension: 0.3, fill: false, pointRadius: 0, spanGaps: true,
      hidden: combinedHidden.has(sid),
    };
  });

  document.getElementById('combined-empty').style.display = sensorIds.length ? 'none' : 'inline-block';
  chart.update('none');
}

function setViewMode(mode) {
  viewMode = mode;
  try { localStorage.setItem(VIEWMODE_KEY, mode); } catch {}
  const combined = mode === 'combined';
  document.getElementById('sensors-grid').style.display  = combined ? 'none' : '';
  document.getElementById('combined-wrap').style.display = combined ? ''     : 'none';
  document.getElementById('organize-btn').style.display  = combined ? 'none' : '';
  const btn = document.getElementById('viewmode-btn');
  btn.textContent = combined ? '⊞ Per-sensor' : '▦ Combined';
  btn.title       = combined ? 'Switch back to per-sensor cards' : 'Switch to a single combined graph';
  if (combined) { ensureCombinedChart(); syncCombined(); }
}
