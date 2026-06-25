// ---------- Central state ----------
const state = {
  flap: {
    id: 'flap_0',
    isUp: true,
    latched: false,
    mode: 'auto',
    threshold: 60,
    raw: 0,
    envelope: 0,
    lastTriggerRaw: -1,
    lastTriggerEnvelope: -1,
    lastTriggerAt: 0,
  },
  network: { ssid: '-', ip: '-', channel: '-', clients: '-', mac: '-' },
  connected: false,
};

// ---------- DOM helpers ----------
function text(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function badge(el, label, modifier) {
  if (!el) return;
  el.textContent = label;
  el.className = el.className.replace(/\bbadge--\S+/g, '').trim();
  if (modifier) el.classList.add('badge--' + modifier);
}

function setControlsDisabled(disabled) {
  document.querySelectorAll('[data-action]').forEach((btn) => {
    btn.disabled = disabled;
  });
  const applyBtn = document.getElementById('applyThresholdBtn');
  if (applyBtn) {
    applyBtn.disabled = disabled;
  }
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

// ---------- Render ----------
function render() {
  const f = state.flap;
  const net = state.network;

  // Network
  text('apSsid', net.ssid);
  text('apIp', net.ip);
  text('apChannel', net.channel);
  text('apClients', net.clients);
  text('apMac', net.mac);

  // Flap SVG
  const flapVisual = document.getElementById('flapVisual');
  if (flapVisual) {
    flapVisual.classList.toggle('up', f.isUp);
    flapVisual.classList.toggle('down', !f.isUp);
  }

  // Summary pills
  text('flapState', f.isUp ? 'HOCH' : 'GEFALLEN');
  badge(document.getElementById('flapMode'),
    f.mode === 'manual' ? 'MANUAL' : 'AUTO',
    f.mode === 'manual' ? 'warn' : 'ok');
  badge(document.getElementById('flapLatch'),
    f.latched ? 'LOCKED' : 'FREE',
    f.latched ? 'danger' : 'ok');
  text('flapThreshold', f.threshold);
  text('lastTriggerRaw', f.lastTriggerRaw >= 0 ? f.lastTriggerRaw : '-');

  // Detail panel
  text('detailState', f.isUp ? 'HOCH' : 'GEFALLEN');
  text('detailMode', f.mode.toUpperCase());
  text('detailTriggerRaw', f.lastTriggerRaw >= 0 ? f.lastTriggerRaw : '-');
  text('detailTriggerEnvelope', f.lastTriggerEnvelope >= 0 ? f.lastTriggerEnvelope : '-');
  text('detailTriggerAt', f.lastTriggerAt > 0 ? f.lastTriggerAt + ' ms' : '-');

  const thresholdRange = document.getElementById('thresholdRange');
  const thresholdNumber = document.getElementById('thresholdNumber');
  if (thresholdRange) thresholdRange.value = String(f.threshold);
  if (thresholdNumber) thresholdNumber.value = String(f.threshold);
}

function applyStatus(data) {
  const f = state.flap;
  f.isUp          = Number(data.flap) === 1;
  f.latched       = Number(data.latched) === 1;
  f.mode          = data.mode || 'auto';
  f.threshold     = Number.isFinite(Number(data.threshold)) ? Number(data.threshold) : f.threshold;
  f.raw           = Number(data.raw) || 0;
  f.envelope      = Number(data.envelope) || 0;
  f.lastTriggerRaw      = Number(data.lastTriggerRaw) ?? -1;
  f.lastTriggerEnvelope = Number(data.lastTriggerEnvelope) ?? -1;
  f.lastTriggerAt       = Number(data.lastTriggerAt) || 0;

  state.network = {
    ssid: data.ssid ?? '-',
    ip: data.ip ?? '-',
    channel: data.channel ?? '-',
    clients: data.clients ?? '-',
    mac: data.mac ?? '-',
  };

  render();
}

// ---------- HTTP polling ----------
function updateStatus() {
  fetch('/api/status')
    .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(data => {
      state.connected = true;
      applyStatus(data);
      text('connectionStatus', 'Status OK');
    })
    .catch(() => {
      state.connected = false;
      text('connectionStatus', 'Connection lost');
    });
}

// ---------- SSE with auto-reconnect ----------
let sseBackoffMs = 1000;
let sseSource = null;

function connectSSE() {
  if (sseSource) {
    sseSource.close();
    sseSource = null;
  }
  const es = new EventSource('/events');
  sseSource = es;

  es.addEventListener('status', (e) => {
    try {
      sseBackoffMs = 1000;
      state.connected = true;
      applyStatus(JSON.parse(e.data));
      text('connectionStatus', 'Live');
    } catch (_) {}
  });

  es.onerror = () => {
    state.connected = false;
    text('connectionStatus', 'SSE offline – retry in ' + (sseBackoffMs / 1000).toFixed(0) + 's');
    es.close();
    sseSource = null;
    const delay = sseBackoffMs;
    sseBackoffMs = Math.min(sseBackoffMs * 2, 30000);
    setTimeout(connectSSE, delay);
  };
}

updateStatus();
connectSSE();

document.querySelectorAll('[data-action]').forEach((btn) => {
  btn.addEventListener('click', async () => {
    try {
      await postFlapAction(btn.dataset.action);
      text('connectionStatus', 'Action sent');
    } catch (err) {
      console.error(err);
      text('connectionStatus', 'Action failed');
    }
  });
});

const thresholdRange = document.getElementById('thresholdRange');
const thresholdNumber = document.getElementById('thresholdNumber');
if (thresholdRange && thresholdNumber) {
  thresholdRange.addEventListener('input', () => {
    thresholdNumber.value = thresholdRange.value;
  });
  thresholdNumber.addEventListener('input', () => {
    const v = Math.max(0, Math.min(1023, Number(thresholdNumber.value) || 0));
    thresholdRange.value = String(v);
  });
}

const applyThresholdBtn = document.getElementById('applyThresholdBtn');
if (applyThresholdBtn) {
  applyThresholdBtn.addEventListener('click', async () => {
    try {
      await postThreshold(thresholdNumber ? thresholdNumber.value : 0);
      text('connectionStatus', 'Threshold updated');
    } catch (err) {
      console.error(err);
      text('connectionStatus', 'Threshold update failed');
    }
  });
}

const flapVisual = document.getElementById('flapVisual');
if (flapVisual) {
  flapVisual.addEventListener('click', () => {
    const panel = document.getElementById('flapDetailPanel');
    const isOpen = panel && panel.classList.contains('open');
    setDetailOpen(!isOpen);
  });
  flapVisual.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      const panel = document.getElementById('flapDetailPanel');
      const isOpen = panel && panel.classList.contains('open');
      setDetailOpen(!isOpen);
    }
  });
}

const closeDetailBtn = document.getElementById('closeDetailBtn');
if (closeDetailBtn) {
  closeDetailBtn.addEventListener('click', () => {
    setDetailOpen(false);
  });
}

setInterval(updateStatus, 5000);
