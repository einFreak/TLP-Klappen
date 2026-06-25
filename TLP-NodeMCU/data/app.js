// ============================================================
// TLP-Klappen Dashboard — App State & Rendering
// Multi-flap-ready, localStorage-persistent, SSE-driven
// ============================================================

// Central app state: flaps array + network info + connection status
const state = {
  flaps: [
    {
      id: 'flap_0',
      label: 'Klappe 1',
      isUp: true,
      latched: false,
      mode: 'auto',
      threshold: 60,
      raw: 0,
      envelope: 0,
      lastTriggerRaw: -1,
      lastTriggerEnvelope: -1,
      lastTriggerAt: 0,
      detailOpen: false,
    },
  ],
  network: { ssid: '-', ip: '-', channel: '-', clients: '-', mac: '-' },
  connected: false,
};

// localStorage key for layout persistence
const LS_KEY = 'tlp_layout';

// Restore detail panel open/close state from localStorage
function loadLayout() {
  try {
    const stored = JSON.parse(localStorage.getItem(LS_KEY) || '{}');
    state.flaps.forEach((f) => {
      const saved = stored[f.id];
      if (saved && typeof saved.detailOpen === 'boolean') {
        f.detailOpen = saved.detailOpen;
      }
    });
  } catch (_) {}
}

// Save detail panel open/close state to localStorage
function saveLayout() {
  const layout = {};
  state.flaps.forEach((f) => {
    layout[f.id] = { detailOpen: f.detailOpen };
  });
  try {
    localStorage.setItem(LS_KEY, JSON.stringify(layout));
  } catch (_) {}
}

// Safe text content update by element ID
function text(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

// Update badge styling (color coding: ok/warn/danger)
function badge(el, label, modifier) {
  if (!el) return;
  el.textContent = label;
  el.className = el.className.replace(/\bbadge--\S+/g, '').trim();
  if (modifier) el.classList.add('badge--' + modifier);
}

// Clone <template id="flap-card-template">, populate IDs, wire threshold inputs
function createCardElement(flap) {
  const tmpl = document.querySelector('#flap-card-template');
  const card = tmpl.content.cloneNode(true).querySelector('.flap-card');
  
  // Populate data attributes (IDs for all interactive elements)
  card.dataset.flapId = flap.id;
  card.querySelector('[data-flap-visual]').dataset.flapVisual = flap.id;
  card.querySelector('[data-close-detail]').dataset.closeDetail = flap.id;
  card.querySelectorAll('[data-apply-threshold]').forEach(el => el.dataset.applyThreshold = flap.id);
  card.querySelectorAll('[data-action]').forEach(el => el.dataset.flapId = flap.id);
  qf(card, 'cardLabel').textContent = flap.label;

  // Wire threshold range ↔ number input sync (DRY: update both when either changes)
  const rangeEl = qf(card, 'thresholdRange');
  const numEl = qf(card, 'thresholdNumber');
  if (rangeEl && numEl) {
    rangeEl.addEventListener('input', () => { numEl.value = rangeEl.value; });
    numEl.addEventListener('input', () => {
      const v = Math.max(0, Math.min(1023, Number(numEl.value) || 0));
      rangeEl.value = String(v);
    });
  }
  return card;
}

// Helper: scoped querySelector by [data-field] (single responsibility: find fields in card)
function qf(parent, field) {
  return parent.querySelector(`[data-field="${field}"]`);
}

async function postFlapAction(action) {
  setControlsDisabled(true);
  try {
    const res = await fetch('/api/flap?action=' + encodeURIComponent(action), { method: 'POST' });
    if (!res.ok) {
      throw new Error('flap action failed');
    }
    applyStatus(await res.json());
  } finally {
    setControlsDisabled(false);
  }
}

async function postThreshold(value) {
  setControlsDisabled(true);
  try {
    const v = Math.max(0, Math.min(1023, Number(value) || 0));
    const res = await fetch('/api/threshold?value=' + encodeURIComponent(v), { method: 'POST' });
    if (!res.ok) {
      throw new Error('threshold update failed');
    }
    applyStatus(await res.json());
  } finally {
    setControlsDisabled(false);
  }
}

function setDetailOpen(isOpen) {
  const panel = document.getElementById('flapDetailPanel');
  if (!panel) {
    return;
  }
  panel.classList.toggle('open', isOpen);
  panel.setAttribute('aria-hidden', isOpen ? 'false' : 'true');
}

// Update single card DOM (SVG, pills, detail panel, threshold)
function renderCard(flap) {
  const stage = document.getElementById('flapStage');
  if (!stage) return;

  let card = stage.querySelector(`[data-flap-id="${flap.id}"]`);
  if (!card) {
    card = createCardElement(flap);
    stage.appendChild(card);
  }

  // Update flap SVG state (up/down classes)
  const visual = card.querySelector('[data-flap-visual]');
  if (visual) {
    visual.classList.toggle('up', flap.isUp);
    visual.classList.toggle('down', !flap.isUp);
  }

  // Update summary metric pills
  const stateEl = qf(card, 'flapState');
  if (stateEl) stateEl.textContent = flap.isUp ? 'HOCH' : 'GEFALLEN';
  badge(qf(card, 'flapMode'), flap.mode === 'manual' ? 'MANUAL' : 'AUTO', flap.mode === 'manual' ? 'warn' : 'ok');
  badge(qf(card, 'flapLatch'), flap.latched ? 'LOCKED' : 'FREE', flap.latched ? 'danger' : 'ok');
  if (qf(card, 'flapThreshold')) qf(card, 'flapThreshold').textContent = flap.threshold;
  if (qf(card, 'lastTriggerRaw')) qf(card, 'lastTriggerRaw').textContent = flap.lastTriggerRaw >= 0 ? flap.lastTriggerRaw : '-';

  // Update detail panel (visibility + all fields)
  const panel = card.querySelector('.flap-detail-panel');
  if (panel) {
    panel.classList.toggle('open', flap.detailOpen);
    panel.setAttribute('aria-hidden', flap.detailOpen ? 'false' : 'true');
  }
  if (qf(card, 'detailState')) qf(card, 'detailState').textContent = flap.isUp ? 'HOCH' : 'GEFALLEN';
  if (qf(card, 'detailMode')) qf(card, 'detailMode').textContent = flap.mode.toUpperCase();
  if (qf(card, 'detailTriggerRaw')) qf(card, 'detailTriggerRaw').textContent = flap.lastTriggerRaw >= 0 ? flap.lastTriggerRaw : '-';
  if (qf(card, 'detailTriggerEnvelope')) qf(card, 'detailTriggerEnvelope').textContent = flap.lastTriggerEnvelope >= 0 ? flap.lastTriggerEnvelope : '-';
  if (qf(card, 'detailTriggerAt')) qf(card, 'detailTriggerAt').textContent = flap.lastTriggerAt > 0 ? flap.lastTriggerAt + ' ms' : '-';

  // Update threshold inputs (skip if user is currently editing to avoid interruption)
  const rangeEl = qf(card, 'thresholdRange');
  const numEl = qf(card, 'thresholdNumber');
  if (rangeEl && document.activeElement !== rangeEl) rangeEl.value = String(flap.threshold);
  if (numEl && document.activeElement !== numEl) numEl.value = String(flap.threshold);
}

// Render all flap cards; remove obsolete cards
function renderStage() {
  const stage = document.getElementById('flapStage');
  if (!stage) return;
  state.flaps.forEach(renderCard);
  stage.querySelectorAll('[data-flap-id]').forEach((card) => {
    if (!state.flaps.find((f) => f.id === card.dataset.flapId)) card.remove();
  });
}

// Render network panel (SSID, IP, channel, etc.)
function renderNetwork() {
  const net = state.network;
  text('apSsid', net.ssid);
  text('apIp', net.ip);
  text('apChannel', net.channel);
  text('apClients', net.clients);
  text('apMac', net.mac);
}

// Main render entry point: update stage + network
function render() {
  renderStage();
  renderNetwork();
}

// Parse API response into state.flaps[0], then trigger render
function applyStatus(data) {
  const f = state.flaps[0]; // Single-flap firmware: always map to first flap
  f.isUp = Number(data.flap) === 1;
  f.latched = Number(data.latched) === 1;
  f.mode = data.mode || 'auto';
  f.threshold = Number.isFinite(Number(data.threshold)) ? Number(data.threshold) : f.threshold;
  f.raw = Number(data.raw) || 0;
  f.envelope = Number(data.envelope) || 0;
  f.lastTriggerRaw = Number(data.lastTriggerRaw) ?? -1;
  f.lastTriggerEnvelope = Number(data.lastTriggerEnvelope) ?? -1;
  f.lastTriggerAt = Number(data.lastTriggerAt) || 0;

  state.network = {
    ssid: data.ssid ?? '-',
    ip: data.ip ?? '-',
    channel: data.channel ?? '-',
    clients: data.clients ?? '-',
    mac: data.mac ?? '-',
  };

  render();
}

// Disable action + threshold buttons during API call
function setCardBusy(card, busy) {
  if (!card) return;
  card.querySelectorAll('[data-action], [data-apply-threshold]').forEach((el) => {
    el.disabled = busy;
  });
}

// POST flap action (up/down/toggle/auto/reset), update UI on success
async function postFlapAction(flapId, action) {
  const stage = document.getElementById('flapStage');
  const card = stage && stage.querySelector(`[data-flap-id="${flapId}"]`);
  setCardBusy(card, true);
  try {
    const res = await fetch('/api/flap?action=' + encodeURIComponent(action), { method: 'POST' });
    if (!res.ok) throw new Error('flap action failed');
    applyStatus(await res.json());
    text('connectionStatus', 'Aktion ausgeführt');
  } catch (err) {
    console.error(err);
    text('connectionStatus', 'Aktion fehlgeschlagen');
  } finally {
    setCardBusy(card, false);
  }
}

// POST threshold value update, update UI on success
async function postThreshold(flapId, value) {
  const stage = document.getElementById('flapStage');
  const card = stage && stage.querySelector(`[data-flap-id="${flapId}"]`);
  setCardBusy(card, true);
  try {
    const v = Math.max(0, Math.min(1023, Number(value) || 0));
    const res = await fetch('/api/threshold?value=' + encodeURIComponent(v), { method: 'POST' });
    if (!res.ok) throw new Error('threshold update failed');
    applyStatus(await res.json());
    text('connectionStatus', 'Schwellwert gesetzt');
  } catch (err) {
    console.error(err);
    text('connectionStatus', 'Schwellwert fehlgeschlagen');
  } finally {
    setCardBusy(card, false);
  }
}

// All interactions delegated to #flapStage (click & keyboard)
document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('flapStage');
  if (!stage) return;

  stage.addEventListener('click', (e) => {
    // Action buttons: up, down, toggle, auto, reset
    const actionBtn = e.target.closest('[data-action]');
    if (actionBtn && actionBtn.dataset.flapId) {
      postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
      return;
    }

    // Threshold "Setzen" button
    const applyBtn = e.target.closest('[data-apply-threshold]');
    if (applyBtn && applyBtn.dataset.applyThreshold) {
      const card = stage.querySelector(`[data-flap-id="${applyBtn.dataset.applyThreshold}"]`);
      const numEl = card && card.querySelector('[data-field="thresholdNumber"]');
      postThreshold(applyBtn.dataset.applyThreshold, numEl ? numEl.value : 0);
      return;
    }

    // Close detail panel (✕ button)
    const closeBtn = e.target.closest('[data-close-detail]');
    if (closeBtn && closeBtn.dataset.closeDetail) {
      const flap = state.flaps.find((f) => f.id === closeBtn.dataset.closeDetail);
      if (flap) { flap.detailOpen = false; saveLayout(); renderCard(flap); }
      return;
    }

    // Toggle detail panel (click on flap visual)
    const visual = e.target.closest('[data-flap-visual]');
    if (visual && visual.dataset.flapVisual) {
      const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
      if (flap) { flap.detailOpen = !flap.detailOpen; saveLayout(); renderCard(flap); }
    }
  });

  // Keyboard: Enter/Space on flap visual toggles detail panel
  stage.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter' && e.key !== ' ') return;
    const visual = e.target.closest('[data-flap-visual]');
    if (visual && visual.dataset.flapVisual) {
      e.preventDefault();
      const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
      if (flap) { flap.detailOpen = !flap.detailOpen; saveLayout(); renderCard(flap); }
    }
  });
});

// Fetch status via GET /api/status (fallback if SSE down)
function updateStatus() {
  fetch('/api/status')
    .then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then((data) => {
      state.connected = true;
      applyStatus(data);
      text('connectionStatus', 'Status OK');
    })
    .catch(() => {
      state.connected = false;
      text('connectionStatus', 'Verbindung verloren');
    });
}

// Connect to SSE /events stream with exponential backoff (1s → 30s max)
let sseBackoffMs = 1000;
let sseSource = null;

function connectSSE() {
  if (sseSource) { sseSource.close(); sseSource = null; }
  const es = new EventSource('/events');
  sseSource = es;

  es.addEventListener('status', (e) => {
    try {
      sseBackoffMs = 1000; // Reset backoff on successful message
      state.connected = true;
      applyStatus(JSON.parse(e.data));
      text('connectionStatus', 'Live');
    } catch (_) {}
  });

  es.onerror = () => {
    state.connected = false;
    text('connectionStatus', 'SSE offline – Retry in ' + (sseBackoffMs / 1000).toFixed(0) + 's');
    es.close();
    sseSource = null;
    const delay = sseBackoffMs;
    sseBackoffMs = Math.min(sseBackoffMs * 2, 30000);
    setTimeout(connectSSE, delay);
  };
}

// Boot app: restore layout, fetch initial status, connect SSE + start polling
loadLayout();
updateStatus();
connectSSE();
setInterval(updateStatus, 5000);

