// ============================================================
// TLP-Klappen Dashboard - Multi-Node State & Rendering
// ============================================================

const DEFAULT_NODE_ID = 0x11;
const LS_KEY = 'tlp_layout';

const state = {
  flaps: [],
  network: { ssid: '-', ip: '-', channel: '-', clients: '-', mac: '-' },
  connected: false,
};

function toNodeId(value, fallback = DEFAULT_NODE_ID) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  const i = Math.trunc(n);
  return i >= 0 && i <= 31 ? i : fallback;
}

function flapIdFromNode(nodeId) {
  return 'node_' + String(nodeId);
}

function nodeLabel(nodeId) {
  return 'Node 0x' + nodeId.toString(16).toUpperCase().padStart(2, '0');
}

function text(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function qf(parent, field) {
  return parent.querySelector('[data-field="' + field + '"]');
}

function badge(el, label, modifier) {
  if (!el) return;
  el.textContent = label;
  el.className = el.className.replace(/\bbadge--\S+/g, '').trim();
  if (modifier) el.classList.add('badge--' + modifier);
}

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

function saveLayout() {
  const layout = {};
  state.flaps.forEach((f) => {
    layout[f.id] = { detailOpen: !!f.detailOpen };
  });
  try {
    localStorage.setItem(LS_KEY, JSON.stringify(layout));
  } catch (_) {}
}

function ensureFlapModel(nodeId) {
  const id = flapIdFromNode(nodeId);
  let flap = state.flaps.find((f) => f.id === id);
  if (!flap) {
    flap = {
      id,
      nodeId,
      label: nodeLabel(nodeId),
      isUp: true,
      latched: false,
      mode: 'auto',
      online: false,
      threshold: 60,
      raw: 0,
      envelope: 0,
      lastTriggerRaw: -1,
      lastTriggerEnvelope: -1,
      lastTriggerAt: 0,
      heartbeat: 0,
      reboots: 0,
      flags: 0,
      uptimeSeconds: 0,
      detailOpen: false,
    };
    state.flaps.push(flap);
  }
  flap.nodeId = nodeId;
  flap.label = nodeLabel(nodeId);
  return flap;
}

function createCardElement(flap) {
  const tmpl = document.querySelector('#flap-card-template');
  const card = tmpl.content.cloneNode(true).querySelector('.flap-card');

  card.dataset.flapId = flap.id;
  card.dataset.nodeId = String(flap.nodeId);
  card.querySelector('[data-flap-visual]').dataset.flapVisual = flap.id;
  card.querySelector('[data-close-detail]').dataset.closeDetail = flap.id;
  card.querySelectorAll('[data-apply-threshold]').forEach((el) => {
    el.dataset.applyThreshold = flap.id;
  });
  card.querySelectorAll('[data-action]').forEach((el) => {
    el.dataset.flapId = flap.id;
  });
  qf(card, 'cardLabel').textContent = flap.label;

  const rangeEl = qf(card, 'thresholdRange');
  const numEl = qf(card, 'thresholdNumber');
  if (rangeEl && numEl) {
    rangeEl.addEventListener('input', () => {
      numEl.value = rangeEl.value;
    });
    numEl.addEventListener('input', () => {
      const v = Math.max(0, Math.min(1023, Number(numEl.value) || 0));
      rangeEl.value = String(v);
    });
  }

  return card;
}

function setCardBusy(card, busy) {
  if (!card) return;
  card.querySelectorAll('[data-action], [data-apply-threshold]').forEach((el) => {
    el.disabled = busy;
  });
}

function renderCard(flap) {
  const stage = document.getElementById('flapStage');
  if (!stage) return;

  let card = stage.querySelector('[data-flap-id="' + flap.id + '"]');
  if (!card) {
    card = createCardElement(flap);
    stage.appendChild(card);
  }

  card.dataset.nodeId = String(flap.nodeId);
  qf(card, 'cardLabel').textContent = flap.label;

  const visual = card.querySelector('[data-flap-visual]');
  if (visual) {
    visual.classList.toggle('up', flap.isUp);
    visual.classList.toggle('down', !flap.isUp);
  }

  const stateEl = qf(card, 'flapState');
  if (stateEl) stateEl.textContent = flap.isUp ? 'HOCH' : 'GEFALLEN';

  badge(qf(card, 'flapMode'), flap.mode === 'manual' ? 'MANUAL' : 'AUTO', flap.mode === 'manual' ? 'warn' : 'ok');
  badge(qf(card, 'flapOnline'), flap.online ? 'ONLINE' : 'OFFLINE', flap.online ? 'ok' : 'danger');

  if (qf(card, 'flapThreshold')) qf(card, 'flapThreshold').textContent = flap.threshold;
  if (qf(card, 'lastTriggerRaw')) qf(card, 'lastTriggerRaw').textContent = flap.lastTriggerRaw >= 0 ? flap.lastTriggerRaw : '-';

  const panel = card.querySelector('.flap-detail-panel');
  if (panel) {
    panel.classList.toggle('open', !!flap.detailOpen);
    panel.setAttribute('aria-hidden', flap.detailOpen ? 'false' : 'true');
  }

  if (qf(card, 'detailNodeId')) qf(card, 'detailNodeId').textContent = String(flap.nodeId);
  if (qf(card, 'detailOnline')) qf(card, 'detailOnline').textContent = flap.online ? 'ja' : 'nein';
  if (qf(card, 'detailHeartbeat')) qf(card, 'detailHeartbeat').textContent = String(flap.heartbeat);
  if (qf(card, 'detailReboots')) qf(card, 'detailReboots').textContent = String(flap.reboots);
  if (qf(card, 'detailFlags')) qf(card, 'detailFlags').textContent = String(flap.flags);
  if (qf(card, 'detailUptime')) qf(card, 'detailUptime').textContent = String(flap.uptimeSeconds);

  if (qf(card, 'detailState')) qf(card, 'detailState').textContent = flap.isUp ? 'HOCH' : 'GEFALLEN';
  if (qf(card, 'detailMode')) qf(card, 'detailMode').textContent = flap.mode.toUpperCase();
  if (qf(card, 'detailTriggerRaw')) qf(card, 'detailTriggerRaw').textContent = flap.lastTriggerRaw >= 0 ? flap.lastTriggerRaw : '-';
  if (qf(card, 'detailTriggerEnvelope')) qf(card, 'detailTriggerEnvelope').textContent = flap.lastTriggerEnvelope >= 0 ? flap.lastTriggerEnvelope : '-';
  if (qf(card, 'detailTriggerAt')) qf(card, 'detailTriggerAt').textContent = flap.lastTriggerAt > 0 ? String(flap.lastTriggerAt) + ' ms' : '-';

  const rangeEl = qf(card, 'thresholdRange');
  const numEl = qf(card, 'thresholdNumber');
  if (rangeEl && document.activeElement !== rangeEl) rangeEl.value = String(flap.threshold);
  if (numEl && document.activeElement !== numEl) numEl.value = String(flap.threshold);
}

function renderStage() {
  const stage = document.getElementById('flapStage');
  if (!stage) return;

  state.flaps.sort((a, b) => a.nodeId - b.nodeId);
  state.flaps.forEach(renderCard);

  stage.querySelectorAll('[data-flap-id]').forEach((card) => {
    if (!state.flaps.find((f) => f.id === card.dataset.flapId)) {
      card.remove();
    }
  });
}

function renderNetwork() {
  const net = state.network;
  text('apSsid', net.ssid);
  text('apIp', net.ip);
  text('apChannel', net.channel);
  text('apClients', net.clients);
  text('apMac', net.mac);
}

function render() {
  renderStage();
  renderNetwork();
}

function computeNodeFlapState(node) {
  const flags = Number(node.flags) || 0;
  const isRelayPulseActive = (flags & (1 << 2)) !== 0;
  const explicitFlapUp = (flags & (1 << 0)) !== 0;
  return explicitFlapUp || !isRelayPulseActive;
}

function parseNodeList(data) {
  if (Array.isArray(data.nodes) && data.nodes.length > 0) {
    return data.nodes;
  }

  if (data.node11 && Number(data.node11.seen) === 1) {
    return [{
      id: DEFAULT_NODE_ID,
      online: Number(data.node11.online) === 1 ? 1 : 0,
      hb: Number(data.node11.heartbeatCount) || 0,
      reboots: Number(data.node11.rebootCount) || 0,
      flags: Number(data.node11.statusFlags) || 0,
      uptimeSeconds: Number(data.node11.uptimeSeconds) || 0,
    }];
  }

  return [];
}

function applyStatus(data) {
  const nodes = parseNodeList(data);
  const keep = new Set();

  if (nodes.length === 0) {
    const fallback = ensureFlapModel(DEFAULT_NODE_ID);
    keep.add(fallback.id);
    fallback.isUp = Number(data.flap) === 1;
    fallback.online = Number(data?.node11?.online) === 1;
    fallback.mode = data.mode || 'auto';
    fallback.threshold = Number.isFinite(Number(data.threshold)) ? Number(data.threshold) : fallback.threshold;
    fallback.raw = Number(data.raw) || 0;
    fallback.envelope = Number(data.envelope) || 0;
    fallback.lastTriggerRaw = Number(data.lastTriggerRaw) ?? -1;
    fallback.lastTriggerEnvelope = Number(data.lastTriggerEnvelope) ?? -1;
    fallback.lastTriggerAt = Number(data.lastTriggerAt) || 0;
    fallback.heartbeat = Number(data?.node11?.heartbeatCount) || fallback.heartbeat;
    fallback.reboots = Number(data?.node11?.rebootCount) || fallback.reboots;
    fallback.flags = Number(data?.node11?.statusFlags) || 0;
    fallback.uptimeSeconds = Number(data?.node11?.uptimeSeconds) || 0;
  } else {
    nodes.forEach((node) => {
      const nodeId = toNodeId(node.id, DEFAULT_NODE_ID);
      const flap = ensureFlapModel(nodeId);
      keep.add(flap.id);

      flap.online = Number(node.online) === 1;
      flap.heartbeat = Number(node.hb) || 0;
      flap.reboots = Number(node.reboots) || 0;
      flap.flags = Number(node.flags) || 0;
      flap.uptimeSeconds = Number(node.uptimeSeconds) || 0;

      flap.isUp = computeNodeFlapState(node);
      flap.mode = (flap.flags & (1 << 1)) !== 0 ? 'auto' : 'manual';

      if (nodeId === DEFAULT_NODE_ID) {
        flap.threshold = Number.isFinite(Number(data.threshold)) ? Number(data.threshold) : flap.threshold;
        flap.raw = Number(data.raw) || 0;
        flap.envelope = Number(data.envelope) || 0;
        flap.lastTriggerRaw = Number(data.lastTriggerRaw) ?? -1;
        flap.lastTriggerEnvelope = Number(data.lastTriggerEnvelope) ?? -1;
        flap.lastTriggerAt = Number(data.lastTriggerAt) || 0;
      } else {
        flap.lastTriggerRaw = -1;
        flap.lastTriggerEnvelope = -1;
        flap.lastTriggerAt = 0;
      }
    });

    state.flaps = state.flaps.filter((f) => keep.has(f.id));
  }

  state.network = {
    ssid: data.ssid ?? '-',
    ip: data.ip ?? '-',
    channel: data.channel ?? '-',
    clients: data.clients ?? '-',
    mac: data.mac ?? '-',
  };

  render();
}

async function waitForCommandResult(requestId, timeoutMs = 1500) {
  const start = Date.now();

  while ((Date.now() - start) < timeoutMs) {
    const res = await fetch('/api/command-result?id=' + encodeURIComponent(requestId));
    if (!res.ok) {
      throw new Error('command result lookup failed');
    }

    const result = await res.json();
    if (Number(result.done) === 1) {
      return result;
    }

    await new Promise((resolve) => setTimeout(resolve, 80));
  }

  throw new Error('command timeout');
}

async function postFlapAction(flapId, action) {
  const stage = document.getElementById('flapStage');
  const card = stage && stage.querySelector('[data-flap-id="' + flapId + '"]');
  const nodeId = toNodeId(card ? card.dataset.nodeId : DEFAULT_NODE_ID);

  setCardBusy(card, true);
  try {
    const res = await fetch('/api/flap?action=' + encodeURIComponent(action) + '&node=' + encodeURIComponent(nodeId), { method: 'POST' });
    if (!res.ok) throw new Error('flap action send failed');

    const accepted = await res.json();
    if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
      throw new Error('invalid command accept response');
    }

    const result = await waitForCommandResult(accepted.requestId, 2000);
    if (result.state !== 'ack') {
      throw new Error('flap action rejected: ' + result.state);
    }

    updateStatus();
    text('connectionStatus', 'Aktion fuer Node ' + nodeId + ' ausgefuehrt');
  } catch (err) {
    console.error(err);
    text('connectionStatus', 'Aktion fehlgeschlagen');
  } finally {
    setCardBusy(card, false);
  }
}

async function postThreshold(flapId, value) {
  const stage = document.getElementById('flapStage');
  const card = stage && stage.querySelector('[data-flap-id="' + flapId + '"]');
  const nodeId = toNodeId(card ? card.dataset.nodeId : DEFAULT_NODE_ID);

  setCardBusy(card, true);
  try {
    const v = Math.max(0, Math.min(1023, Number(value) || 0));
    const res = await fetch('/api/threshold?value=' + encodeURIComponent(v) + '&node=' + encodeURIComponent(nodeId), { method: 'POST' });
    if (!res.ok) throw new Error('threshold update send failed');

    const accepted = await res.json();
    if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
      throw new Error('invalid command accept response');
    }

    const result = await waitForCommandResult(accepted.requestId, 2000);
    if (result.state !== 'ack') {
      throw new Error('threshold rejected: ' + result.state);
    }

    updateStatus();
    text('connectionStatus', 'Schwellwert fuer Node ' + nodeId + ' gesetzt');
  } catch (err) {
    console.error(err);
    text('connectionStatus', 'Schwellwert fehlgeschlagen');
  } finally {
    setCardBusy(card, false);
  }
}

document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('flapStage');
  if (!stage) return;

  stage.addEventListener('click', (e) => {
    const actionBtn = e.target.closest('[data-action]');
    if (actionBtn && actionBtn.dataset.flapId) {
      postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
      return;
    }

    const applyBtn = e.target.closest('[data-apply-threshold]');
    if (applyBtn && applyBtn.dataset.applyThreshold) {
      const card = stage.querySelector('[data-flap-id="' + applyBtn.dataset.applyThreshold + '"]');
      const numEl = card && card.querySelector('[data-field="thresholdNumber"]');
      postThreshold(applyBtn.dataset.applyThreshold, numEl ? numEl.value : 0);
      return;
    }

    const closeBtn = e.target.closest('[data-close-detail]');
    if (closeBtn && closeBtn.dataset.closeDetail) {
      const flap = state.flaps.find((f) => f.id === closeBtn.dataset.closeDetail);
      if (flap) {
        flap.detailOpen = false;
        saveLayout();
        renderCard(flap);
      }
      return;
    }

    const visual = e.target.closest('[data-flap-visual]');
    if (visual && visual.dataset.flapVisual) {
      const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
      if (flap) {
        flap.detailOpen = !flap.detailOpen;
        saveLayout();
        renderCard(flap);
      }
    }
  });

  stage.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter' && e.key !== ' ') return;
    const visual = e.target.closest('[data-flap-visual]');
    if (visual && visual.dataset.flapVisual) {
      e.preventDefault();
      const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
      if (flap) {
        flap.detailOpen = !flap.detailOpen;
        saveLayout();
        renderCard(flap);
      }
    }
  });
});

function updateStatus() {
  fetch('/api/status')
    .then((r) => {
      if (!r.ok) throw new Error(r.status);
      return r.json();
    })
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
    text('connectionStatus', 'SSE offline - Retry in ' + (sseBackoffMs / 1000).toFixed(0) + 's');
    es.close();
    sseSource = null;

    const delay = sseBackoffMs;
    sseBackoffMs = Math.min(sseBackoffMs * 2, 30000);
    setTimeout(connectSSE, delay);
  };
}

ensureFlapModel(DEFAULT_NODE_ID);
loadLayout();
updateStatus();
connectSSE();

setInterval(() => {
  if (!state.connected) {
    updateStatus();
  }
}, 5000);
