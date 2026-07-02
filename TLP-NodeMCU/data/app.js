// ============================================================
// TLP-Klappen Dashboard - Multi-Node State & Rendering
// ============================================================

const LS_KEY = 'tlp_layout';

const state = {
  flaps: [],
  network: { ssid: '-', ip: '-', channel: '-', clients: '-', mac: '-' },
  connected: false,
};

function finiteOr(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function parseTriggerValue(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : -1;
}

function toNodeId(value, fallback = -1) {
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
      if (saved) {
        f.visualLeft = saved.visualLeft || 0;
        f.visualTop = saved.visualTop || 0;
      }
    });
  } catch (_) {}
}

function saveLayout() {
  const layout = {};
  state.flaps.forEach((f) => {
    layout[f.id] = { 
      visualLeft: f.visualLeft || 0, 
      visualTop: f.visualTop || 0 
    };
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
      sensorRaw: 0,
      sensorPeak: 0,
      sensorNoise: 0,
      commandState: 'idle',
      commandText: 'Bereit',
      detailOpen: false,
      visualLeft: 0,
      visualTop: 0,
      isMoving: false,
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

  const visual = card.querySelector('[data-flap-visual]');
  if (visual) {
    visual.dataset.flapVisual = flap.id;
  }

  card.querySelectorAll('[data-action]').forEach((el) => {
    el.dataset.flapId = flap.id;
  });

  const moveBtn = card.querySelector('[data-move-flap]');
  if (moveBtn) {
    moveBtn.dataset.moveFlapId = flap.id;
  }

  return card;
}

function setFlapCommandState(flapId, commandState, commandText) {
  const flap = state.flaps.find((f) => f.id === flapId);
  if (!flap) return;
  flap.commandState = commandState;
  flap.commandText = commandText;
  renderDetailPanel();
}

function renderDetailPanel() {
  const detailPanel = document.getElementById('detailPanel');
  if (!detailPanel) return;

  const openFlap = state.flaps.find((f) => f.detailOpen);
  if (!openFlap) {
    if (detailPanel.contains(document.activeElement)) {
      document.activeElement.blur();
    }
    detailPanel.inert = true;
    detailPanel.setAttribute('aria-hidden', 'true');
    return;
  }

  detailPanel.inert = false;
  detailPanel.setAttribute('aria-hidden', 'false');
  
  // Update all fields
  const fields = {
    cardLabel: openFlap.label,
    detailNodeId: String(openFlap.nodeId),
    detailOnline: openFlap.online ? 'ja' : 'nein',
    detailHeartbeat: String(openFlap.heartbeat),
    detailReboots: String(openFlap.reboots),
    detailFlags: String(openFlap.flags),
    detailUptime: String(openFlap.uptimeSeconds),
    detailState: openFlap.isUp ? 'HOCH' : 'GEFALLEN',
    detailMode: openFlap.mode.toUpperCase(),
    detailSensorRaw: String(openFlap.sensorRaw),
    detailSensorPeak: String(openFlap.sensorPeak),
    detailSensorNoise: String(openFlap.sensorNoise),
  };

  Object.entries(fields).forEach(([field, value]) => {
    const el = qf(detailPanel, field);
    if (el) el.textContent = value;
  });

  // Update command status
  const cmdStatus = qf(detailPanel, 'commandStatus');
  if (cmdStatus) {
    cmdStatus.textContent = openFlap.commandText || 'Bereit';
    cmdStatus.className = 'command-status';
    if (openFlap.commandState === 'pending') cmdStatus.classList.add('command-status--pending');
    else if (openFlap.commandState === 'ok') cmdStatus.classList.add('command-status--ok');
    else if (openFlap.commandState === 'error') cmdStatus.classList.add('command-status--error');
  }

  // Update action buttons
  const disabled = !openFlap.online || openFlap.commandState === 'pending';
  detailPanel.querySelectorAll('[data-action]').forEach((el) => {
    el.dataset.flapId = openFlap.id;
    el.disabled = disabled;
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

  // Apply positioning
  if (flap.visualLeft || flap.visualTop) {
    card.style.position = 'absolute';
    card.style.left = flap.visualLeft + 'px';
    card.style.top = flap.visualTop + 'px';
  } else {
    card.style.position = 'relative';
    card.style.left = 'auto';
    card.style.top = 'auto';
  }

  // Update visual state
  const visual = card.querySelector('[data-flap-visual]');
  if (visual) {
    visual.classList.toggle('up', flap.isUp);
    visual.classList.toggle('down', !flap.isUp);
  }

  // Update move button state
  const moveBtn = card.querySelector('[data-move-flap]');
  if (moveBtn) {
    moveBtn.classList.toggle('moving', flap.isMoving);
  }
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
  if (!Object.prototype.hasOwnProperty.call(data, 'nodes')) {
    return null;
  }
  return Array.isArray(data.nodes) ? data.nodes : [];
}

function applyStatus(data) {
  const nodes = parseNodeList(data);
  if (nodes !== null) {
    const keep = new Set();

    nodes.forEach((node) => {
      const nodeId = toNodeId(node.id, -1);
      if (nodeId < 0) {
        return;
      }

      const flap = ensureFlapModel(nodeId);
      keep.add(flap.id);

      flap.online = Number(node.online) === 1;
      flap.heartbeat = finiteOr(node.hb, 0);
      flap.reboots = finiteOr(node.reboots, 0);
      flap.flags = finiteOr(node.flags, 0);
      flap.uptimeSeconds = finiteOr(node.uptimeSeconds, 0);

      // SENSOR_SUMMARY fields from node
      flap.sensorRaw = finiteOr(node.sensorRaw, flap.sensorRaw);
      flap.sensorPeak = finiteOr(node.sensorPeak, flap.sensorPeak);
      flap.sensorNoise = finiteOr(node.sensorNoise, flap.sensorNoise);

      flap.isUp = computeNodeFlapState(node);
      flap.mode = (flap.flags & (1 << 1)) !== 0 ? 'auto' : 'manual';

      flap.threshold = finiteOr(node.threshold, flap.threshold);
      flap.raw = finiteOr(node.raw, flap.raw);
      flap.envelope = finiteOr(node.envelope, flap.envelope);
      flap.lastTriggerRaw = parseTriggerValue(node.lastTriggerRaw);
      flap.lastTriggerEnvelope = parseTriggerValue(node.lastTriggerEnvelope);
      flap.lastTriggerAt = finiteOr(node.lastTriggerAt, 0);
    });

    state.flaps = state.flaps.filter((f) => keep.has(f.id));

    if (state.flaps.length === 0) {
      text('connectionStatus', 'Keine Nodes im Status vorhanden');
    }
  }

  state.network = {
    ssid: data.ssid ?? '-',
    ip: data.ip ?? '-',
    channel: data.channel ?? '-',
    clients: data.clients ?? '-',
    mac: data.mac ?? '-',
  };

  render();
  renderDetailPanel();
}

document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('flapStage');
  const detailPanel = document.getElementById('detailPanel');
  const closeBtn = document.getElementById('closeDetailBtn');

  // Simplified handler for all card interactions
  if (stage) {
    stage.addEventListener('click', (e) => {
      e.stopPropagation();
      const target = e.target;

      // Check if clicking on SVG - open detail
      if (target.closest('.flap-svg')) {
        const visual = target.closest('[data-flap-visual]');
        if (visual?.dataset.flapVisual) {
          const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
          if (flap) {
            const card = target.closest('.flap-card');
            if (card) {
              stage.querySelectorAll('.flap-card').forEach((c) => c.classList.remove('show-actions'));
              card.classList.add('show-actions');
            }
            state.flaps.forEach((f) => { f.detailOpen = f.id === flap.id; });
            renderDetailPanel();
          }
        }
        return;
      }

      // Check if clicking action button
      const actionBtn = target.closest('[data-action]');
      if (actionBtn?.dataset.flapId) {
        postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
        return;
      }

      // Check if clicking move button
      const moveBtn = target.closest('[data-move-flap]');
      if (moveBtn?.dataset.moveFlapId) {
        const flap = state.flaps.find((f) => f.id === moveBtn.dataset.moveFlapId);
        if (flap) {
          flap.isMoving = !flap.isMoving;
          renderCard(flap);
          if (flap.isMoving) {
            const card = stage.querySelector('[data-flap-id="' + flap.id + '"]');
            setupCardDrag(moveBtn, card, flap);
          }
        }
        return;
      }

      // Default: show/hide buttons on card click
      const card = target.closest('.flap-card');
      if (card) {
        stage.querySelectorAll('.flap-card').forEach((c) => c.classList.remove('show-actions'));
        card.classList.add('show-actions');
      }
    });
  }

  // Close detail panel
  closeBtn?.addEventListener('click', () => {
    if (detailPanel?.contains(document.activeElement)) {
      document.activeElement.blur();
    }
    state.flaps.forEach((f) => { f.detailOpen = false; });
    renderDetailPanel();
  });

  // Detail panel action buttons
  detailPanel?.addEventListener('click', (e) => {
    const actionBtn = e.target.closest('[data-action]');
    if (actionBtn?.dataset.flapId) {
      postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
    }
  });
});

function setupCardDrag(moveBtn, card, flap) {
  let isDragging = false;
  let startX, startY, startLeft, startTop;

  const onMouseDown = (e) => {
    if (!flap.isMoving) return;
    e.preventDefault();
    isDragging = true;
    startX = e.clientX;
    startY = e.clientY;
    startLeft = flap.visualLeft || 0;
    startTop = flap.visualTop || 0;

    const onMouseMove = (e) => {
      if (!isDragging) return;
      const dx = e.clientX - startX;
      const dy = e.clientY - startY;
      const newLeft = Math.max(0, startLeft + dx);
      const newTop = Math.max(0, startTop + dy);
      card.style.position = 'absolute';
      card.style.left = newLeft + 'px';
      card.style.top = newTop + 'px';
    };

    const onMouseUp = () => {
      isDragging = false;
      document.removeEventListener('mousemove', onMouseMove);
      document.removeEventListener('mouseup', onMouseUp);
      flap.visualLeft = parseInt(card.style.left) || 0;
      flap.visualTop = parseInt(card.style.top) || 0;
      saveLayout();
    };

    document.addEventListener('mousemove', onMouseMove);
    document.addEventListener('mouseup', onMouseUp);
  };

  moveBtn.addEventListener('mousedown', onMouseDown);
}

loadLayout();
