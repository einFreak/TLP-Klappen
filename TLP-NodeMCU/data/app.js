// ============================================================
// TLP-Klappen Dashboard - Multi-Node State & Rendering
// ============================================================

const LS_KEY = 'tlp_layout';
const STAGE_HEIGHT_KEY = 'tlp_stage_height';
const KNOWN_FLAPS_KEY = 'tlp_known_flaps';
const UP_TIME_HISTORY_KEY = 'tlp_up_time_history';
const MAX_UP_TIME_SAMPLES = 20;

let persistedLayout = {};
let persistedUpTimeHistory = {};

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
    persistedLayout = (stored && typeof stored === 'object') ? stored : {};
  } catch (_) {
    persistedLayout = {};
  }

  state.flaps.forEach((f) => {
    const saved = persistedLayout[f.id];
    if (!saved) return;
    f.visualLeft = finiteOr(saved.visualLeft, 0);
    f.visualTop = finiteOr(saved.visualTop, 0);
  });
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
    persistedLayout = layout;
  } catch (_) {}
}

function saveKnownFlaps() {
  const knownNodeIds = state.flaps
    .map((f) => toNodeId(f.nodeId, -1))
    .filter((id) => id >= 0);

  const uniqueSorted = Array.from(new Set(knownNodeIds)).sort((a, b) => a - b);
  try {
    localStorage.setItem(KNOWN_FLAPS_KEY, JSON.stringify(uniqueSorted));
  } catch (_) {}
}

function loadUpTimeHistory() {
  try {
    const stored = JSON.parse(localStorage.getItem(UP_TIME_HISTORY_KEY) || '{}');
    persistedUpTimeHistory = (stored && typeof stored === 'object') ? stored : {};
  } catch (_) {
    persistedUpTimeHistory = {};
  }
}

function normalizeHistoryEntry(entry) {
  if (!entry || typeof entry !== 'object') {
    return { samples: [], lastEventCounter: -1 };
  }

  const rawSamples = Array.isArray(entry.samples) ? entry.samples : [];
  const samples = rawSamples
    .map((value) => Number(value))
    .filter((value) => Number.isFinite(value) && value >= 0)
    .map((value) => Math.round(value))
    .slice(-MAX_UP_TIME_SAMPLES);

  const eventCounter = Number(entry.lastEventCounter);
  const lastEventCounter = Number.isFinite(eventCounter)
    ? Math.max(0, Math.min(255, Math.trunc(eventCounter)))
    : -1;

  return { samples, lastEventCounter };
}

function readPersistedHistoryForFlap(flapId) {
  const entry = persistedUpTimeHistory[flapId];
  return normalizeHistoryEntry(entry);
}

function saveUpTimeHistory() {
  const next = {};
  state.flaps.forEach((flap) => {
    const hasSamples = Array.isArray(flap.upTimeHistoryMs) && flap.upTimeHistoryMs.length > 0;
    const hasCounter = Number.isFinite(flap.lastUpEventCounter) && flap.lastUpEventCounter >= 0;
    if (!hasSamples && !hasCounter) {
      return;
    }

    next[flap.id] = {
      samples: hasSamples ? flap.upTimeHistoryMs.slice(-MAX_UP_TIME_SAMPLES) : [],
      lastEventCounter: hasCounter ? Math.trunc(flap.lastUpEventCounter) : -1,
    };
  });

  persistedUpTimeHistory = next;
  try {
    localStorage.setItem(UP_TIME_HISTORY_KEY, JSON.stringify(next));
  } catch (_) {}
}

function computeHistoryStats(samples) {
  if (!Array.isArray(samples) || samples.length === 0) {
    return null;
  }

  const sorted = samples.slice().sort((a, b) => a - b);
  const count = sorted.length;
  const sum = sorted.reduce((acc, value) => acc + value, 0);
  const average = Math.round(sum / count);
  const median = (count % 2 === 1)
    ? sorted[(count - 1) / 2]
    : Math.round((sorted[(count / 2) - 1] + sorted[count / 2]) / 2);

  return { count, average, median };
}

function recordUpTimeSample(flap, upTimeMs, eventCounterRaw) {
  const eventCounter = Number(eventCounterRaw);
  if (!Number.isFinite(eventCounter)) {
    return;
  }

  const normalizedEventCounter = Math.max(0, Math.min(255, Math.trunc(eventCounter)));
  if (flap.lastUpEventCounter === normalizedEventCounter) {
    return;
  }

  flap.lastUpEventCounter = normalizedEventCounter;

  const sample = Number(upTimeMs);
  if (!Number.isFinite(sample) || sample < 0) {
    saveUpTimeHistory();
    return;
  }

  flap.upTimeHistoryMs.push(Math.round(sample));
  if (flap.upTimeHistoryMs.length > MAX_UP_TIME_SAMPLES) {
    flap.upTimeHistoryMs = flap.upTimeHistoryMs.slice(-MAX_UP_TIME_SAMPLES);
  }
  saveUpTimeHistory();
}

function loadKnownFlaps() {
  try {
    const raw = JSON.parse(localStorage.getItem(KNOWN_FLAPS_KEY) || '[]');
    if (!Array.isArray(raw)) return;
    raw.forEach((value) => {
      const nodeId = toNodeId(value, -1);
      if (nodeId >= 0) {
        ensureFlapModel(nodeId);
      }
    });
  } catch (_) {}
}

function ensureFlapModel(nodeId) {
  const id = flapIdFromNode(nodeId);
  let flap = state.flaps.find((f) => f.id === id);
  if (!flap) {
    const persistedHistory = readPersistedHistoryForFlap(id);
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
      lastUpTimeMs: 0,
      lastUpEventCounter: persistedHistory.lastEventCounter,
      upTimeHistoryMs: persistedHistory.samples,
      commandState: 'idle',
      commandText: 'Bereit',
      detailOpen: false,
      visualLeft: 0,
      visualTop: 0,
      isMoving: false,
    };

    const saved = persistedLayout[flap.id];
    if (saved) {
      flap.visualLeft = finiteOr(saved.visualLeft, 0);
      flap.visualTop = finiteOr(saved.visualTop, 0);
    }

    state.flaps.push(flap);
    saveKnownFlaps();
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

  const historyStats = computeHistoryStats(openFlap.upTimeHistoryMs);
  
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
    detailLastUpTime: String(openFlap.lastUpTimeMs) + ' ms',
    detailReactionAvg: historyStats ? String(historyStats.average) + ' ms' : '-',
    detailReactionMedian: historyStats ? String(historyStats.median) + ' ms' : '-',
    detailReactionSamples: historyStats ? String(historyStats.count) : '0',
    detailThreshold: String(openFlap.threshold),
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

  const thresholdInput = qf(detailPanel, 'thresholdInput');
  if (thresholdInput) {
    thresholdInput.value = String(openFlap.threshold);
    thresholdInput.disabled = disabled;
  }

  const thresholdButton = detailPanel.querySelector('[data-apply-threshold]');
  if (thresholdButton) {
    thresholdButton.dataset.flapId = openFlap.id;
    thresholdButton.disabled = disabled;
  }
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
    visual.setAttribute('aria-disabled', flap.online ? 'false' : 'true');
    visual.tabIndex = flap.online ? 0 : -1;
  }

  card.classList.toggle('is-offline', !flap.online);
  if (!flap.online) {
    card.classList.remove('show-actions');
  }

  const quickDisabled = !flap.online || flap.commandState === 'pending';
  card.querySelectorAll('[data-action], [data-move-flap]').forEach((el) => {
    el.disabled = quickDisabled;
  });

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

async function postBulkFlapAction(action) {
  const onlineFlaps = state.flaps.filter((f) => f.online);
  if (onlineFlaps.length === 0) {
    text('connectionStatus', 'Keine Online-Flaps gefunden');
    return;
  }

  text('connectionStatus', 'Sende Sammelaktion...');
  const results = await Promise.allSettled(
    onlineFlaps.map((flap) => postFlapAction(flap.id, action))
  );

  const failed = results.filter((r) => r.status === 'rejected').length;
  if (failed > 0) {
    text('connectionStatus', 'Sammelaktion teilweise fehlgeschlagen (' + failed + ')');
  } else {
    text('connectionStatus', 'Sammelaktion abgeschlossen (' + onlineFlaps.length + ' Flaps)');
  }
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
    state.flaps.forEach((f) => {
      f.online = false;
    });

    nodes.forEach((node) => {
      const nodeId = toNodeId(node.id, -1);
      if (nodeId < 0) {
        return;
      }

      const flap = ensureFlapModel(nodeId);

      flap.online = Number(node.online) === 1;
      flap.heartbeat = finiteOr(node.hb, 0);
      flap.reboots = finiteOr(node.reboots, 0);
      flap.flags = finiteOr(node.flags, 0);
      flap.uptimeSeconds = finiteOr(node.uptimeSeconds, 0);

      // SENSOR_SUMMARY fields from node
      flap.sensorRaw = finiteOr(node.sensorRaw, flap.sensorRaw);
      flap.sensorPeak = finiteOr(node.sensorPeak, flap.sensorPeak);
      flap.sensorNoise = finiteOr(node.sensorNoise, flap.sensorNoise);
      const nextLastUpTimeMs = finiteOr(node.lastUpTimeMs, flap.lastUpTimeMs);
      flap.lastUpTimeMs = nextLastUpTimeMs;
      recordUpTimeSample(flap, nextLastUpTimeMs, node.lastUpEventCounter);

      flap.isUp = computeNodeFlapState(node);
      flap.mode = (flap.flags & (1 << 1)) !== 0 ? 'auto' : 'manual';

      flap.threshold = finiteOr(node.threshold, flap.threshold);
      flap.raw = finiteOr(node.raw, flap.raw);
      flap.envelope = finiteOr(node.envelope, flap.envelope);
      flap.lastTriggerRaw = parseTriggerValue(node.lastTriggerRaw);
      flap.lastTriggerEnvelope = parseTriggerValue(node.lastTriggerEnvelope);
      flap.lastTriggerAt = finiteOr(node.lastTriggerAt, 0);
    });

    saveKnownFlaps();

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
  const stageResizeHandle = document.getElementById('flapStageResizeHandle');
  const allFlapsUpBtn = document.getElementById('allFlapsUpBtn');
  const allFlapsDownBtn = document.getElementById('allFlapsDownBtn');
  const detailPanel = document.getElementById('detailPanel');
  const closeBtn = document.getElementById('closeDetailBtn');

  const dragState = {
    flapId: null,
    pointerId: null,
    startX: 0,
    startY: 0,
    startLeft: 0,
    startTop: 0,
  };

  const stageResizeState = {
    active: false,
    pointerId: null,
    startY: 0,
    startHeight: 0,
  };

  loadLayout();
  loadUpTimeHistory();
  loadKnownFlaps();
  render();
  renderDetailPanel();

  if (stage) {
    const savedStageHeight = Number(localStorage.getItem(STAGE_HEIGHT_KEY));
    if (Number.isFinite(savedStageHeight) && savedStageHeight >= 220) {
      stage.style.height = String(Math.round(savedStageHeight)) + 'px';
    }
  }

  if (stage && stageResizeHandle) {
    stageResizeHandle.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      stageResizeState.active = true;
      stageResizeState.pointerId = e.pointerId;
      stageResizeState.startY = e.clientY;
      stageResizeState.startHeight = stage.getBoundingClientRect().height;
      if (typeof stageResizeHandle.setPointerCapture === 'function') {
        stageResizeHandle.setPointerCapture(e.pointerId);
      }
    });

    const resizeStage = (e) => {
      if (!stageResizeState.active || stageResizeState.pointerId !== e.pointerId) return;
      e.preventDefault();

      const delta = e.clientY - stageResizeState.startY;
      const minHeight = 220;
      const maxHeight = Math.max(minHeight, window.innerHeight - 220);
      const nextHeight = Math.max(minHeight, Math.min(maxHeight, Math.round(stageResizeState.startHeight + delta)));
      stage.style.height = String(nextHeight) + 'px';
    };

    const finishResize = (e) => {
      if (!stageResizeState.active || stageResizeState.pointerId !== e.pointerId) return;
      stageResizeState.active = false;
      stageResizeState.pointerId = null;

      const finalHeight = Math.round(stage.getBoundingClientRect().height);
      try {
        localStorage.setItem(STAGE_HEIGHT_KEY, String(finalHeight));
      } catch (_) {}
    };

    stageResizeHandle.addEventListener('pointermove', resizeStage);
    stageResizeHandle.addEventListener('pointerup', finishResize);
    stageResizeHandle.addEventListener('pointercancel', finishResize);
  }

  allFlapsUpBtn?.addEventListener('click', () => {
    // RESET on ATmega sets flap up and enables AUTO mode.
    postBulkFlapAction('reset');
  });

  allFlapsDownBtn?.addEventListener('click', () => {
    postBulkFlapAction('down');
  });

  // Simplified handler for all card interactions
  if (stage) {
    stage.addEventListener('pointerdown', (e) => {
      const moveBtn = e.target.closest('[data-move-flap]');
      if (!moveBtn?.dataset.moveFlapId) return;
      if (moveBtn.disabled) return;

      const flap = state.flaps.find((f) => f.id === moveBtn.dataset.moveFlapId);
      const card = stage.querySelector('[data-flap-id="' + moveBtn.dataset.moveFlapId + '"]');
      if (!flap || !card || !flap.online) return;

      e.preventDefault();
      e.stopPropagation();

      // Keep quick actions open while dragging this card.
      stage.querySelectorAll('.flap-card').forEach((c) => c.classList.remove('show-actions'));
      card.classList.add('show-actions');

      // Convert the card into absolute positioning at its current visual position.
      const stageRect = stage.getBoundingClientRect();
      const cardRect = card.getBoundingClientRect();
      if (!flap.visualLeft && !flap.visualTop) {
        flap.visualLeft = Math.max(0, Math.round(cardRect.left - stageRect.left + stage.scrollLeft));
        flap.visualTop = Math.max(0, Math.round(cardRect.top - stageRect.top + stage.scrollTop));
      }

      flap.isMoving = true;
      renderCard(flap);

      dragState.flapId = flap.id;
      dragState.pointerId = e.pointerId;
      dragState.startX = e.clientX;
      dragState.startY = e.clientY;
      dragState.startLeft = flap.visualLeft || 0;
      dragState.startTop = flap.visualTop || 0;

      if (typeof moveBtn.setPointerCapture === 'function') {
        moveBtn.setPointerCapture(e.pointerId);
      }
    });

    stage.addEventListener('pointermove', (e) => {
      if (!dragState.flapId || dragState.pointerId !== e.pointerId) return;

      const flap = state.flaps.find((f) => f.id === dragState.flapId);
      const card = stage.querySelector('[data-flap-id="' + dragState.flapId + '"]');
      if (!flap || !card) return;

      e.preventDefault();

      const dx = e.clientX - dragState.startX;
      const dy = e.clientY - dragState.startY;
      const newLeft = Math.max(0, Math.round(dragState.startLeft + dx));
      const newTop = Math.max(0, Math.round(dragState.startTop + dy));

      flap.visualLeft = newLeft;
      flap.visualTop = newTop;

      card.style.position = 'absolute';
      card.style.left = newLeft + 'px';
      card.style.top = newTop + 'px';
    });

    const finishDrag = (e) => {
      if (!dragState.flapId || dragState.pointerId !== e.pointerId) return;

      const flap = state.flaps.find((f) => f.id === dragState.flapId);
      if (flap) {
        flap.isMoving = false;
        renderCard(flap);
        saveLayout();
      }

      dragState.flapId = null;
      dragState.pointerId = null;
    };

    stage.addEventListener('pointerup', finishDrag);
    stage.addEventListener('pointercancel', finishDrag);

    stage.addEventListener('click', (e) => {
      e.stopPropagation();
      const target = e.target;

      const clearSelection = () => {
        stage.querySelectorAll('.flap-card').forEach((c) => c.classList.remove('show-actions'));
        state.flaps.forEach((f) => { f.detailOpen = false; });
        renderDetailPanel();
      };

      // Check if clicking on SVG - open detail
      if (target.closest('.flap-svg')) {
        const visual = target.closest('[data-flap-visual]');
        if (visual?.dataset.flapVisual) {
          const flap = state.flaps.find((f) => f.id === visual.dataset.flapVisual);
          if (flap && flap.online) {
            const card = target.closest('.flap-card');
            const isAlreadySelected = flap.detailOpen && !!card?.classList.contains('show-actions');
            if (isAlreadySelected) {
              clearSelection();
              return;
            }

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
        if (actionBtn.disabled) return;
        postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
        return;
      }

      // Check if clicking move button
      const moveBtn = target.closest('[data-move-flap]');
      if (moveBtn?.dataset.moveFlapId) {
        // Drag is handled by pointer events above.
        return;
      }

      // Default: show/hide buttons on card click
      const card = target.closest('.flap-card');
      if (card) {
        const flap = state.flaps.find((f) => f.id === card.dataset.flapId);
        if (!flap || !flap.online) {
          card.classList.remove('show-actions');
          return;
        }
        const isAlreadySelected = card.classList.contains('show-actions') || !!flap?.detailOpen;
        if (isAlreadySelected) {
          clearSelection();
          return;
        }

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
    const thresholdBtn = e.target.closest('[data-apply-threshold]');
    if (thresholdBtn?.dataset.flapId) {
      if (thresholdBtn.disabled) return;
      const thresholdInput = qf(detailPanel, 'thresholdInput');
      const nextValue = thresholdInput ? Number(thresholdInput.value) : NaN;
      if (!Number.isFinite(nextValue)) {
        setFlapCommandState(thresholdBtn.dataset.flapId, 'error', 'Threshold ungueltig');
        return;
      }
      postThreshold(thresholdBtn.dataset.flapId, nextValue);
      return;
    }

    const actionBtn = e.target.closest('[data-action]');
    if (actionBtn?.dataset.flapId) {
      if (actionBtn.disabled) return;
      postFlapAction(actionBtn.dataset.flapId, actionBtn.dataset.action);
    }
  });

  detailPanel?.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter') return;
    const thresholdInput = e.target.closest('[data-field="thresholdInput"]');
    if (!thresholdInput) return;

    const openFlap = state.flaps.find((f) => f.detailOpen);
    if (!openFlap) return;

    e.preventDefault();
    const nextValue = Number(thresholdInput.value);
    if (!Number.isFinite(nextValue)) {
      setFlapCommandState(openFlap.id, 'error', 'Threshold ungueltig');
      return;
    }
    postThreshold(openFlap.id, nextValue);
  });

  // ============================================================
  // Reaction Test Modal - Single Flap Hit Detection
  // ============================================================

  const reactionTestBtn = document.getElementById('reactionTestBtn');
  const reactionTestModal = document.getElementById('reactionTestModal');
  const reactionTestInitial = document.getElementById('reactionTestInitial');
  const reactionTestWaiting = document.getElementById('reactionTestWaiting');
  const reactionTestSignaled = document.getElementById('reactionTestSignaled');
  const reactionTestResult = document.getElementById('reactionTestResult');
  const reactionTestStartBtn = document.getElementById('reactionTestStartBtn');
  const reactionTestReactBtn = document.getElementById('reactionTestReactBtn');
  const reactionTestAgainBtn = document.getElementById('reactionTestAgainBtn');
  const reactionTestCloseBtns = document.querySelectorAll('#reactionTestCloseBtn, #reactionTestCloseBtn2');
  
  let reactionTestSession = {
    running: false,
    hitsTarget: 5,
    hitsCollected: 0,
    reactionTimes: [],
    currentTargetNodeId: -1,
    lastRecordedUpTimeMs: 0,
  };

  const LS_BEST_TIME_KEY = 'tlp_reaction_best_time';

  function loadAndDisplayBestTime() {
    try {
      const bestTimeStr = localStorage.getItem(LS_BEST_TIME_KEY);
      const bestTime = bestTimeStr ? Number(bestTimeStr) : null;
      const bestTimeEl = document.getElementById('reactionTestBestTime');
      if (bestTimeEl) {
        if (bestTime !== null && Number.isFinite(bestTime)) {
          bestTimeEl.textContent = 'Best: ' + bestTime + ' ms';
        } else {
          bestTimeEl.textContent = '';
        }
      }
    } catch (_) {}
  }

  function saveBestTime(times) {
    if (!Array.isArray(times) || times.length === 0) return;
    
    const average = Math.round(times.reduce((a, b) => a + b, 0) / times.length);
    
    try {
      const bestTimeStr = localStorage.getItem(LS_BEST_TIME_KEY);
      const currentBest = bestTimeStr ? Number(bestTimeStr) : null;
      
      if (currentBest === null || !Number.isFinite(currentBest) || average < currentBest) {
        localStorage.setItem(LS_BEST_TIME_KEY, String(average));
        loadAndDisplayBestTime();
      }
    } catch (_) {}
  }

  function hideAllReactionTestSections() {
    reactionTestInitial.style.display = 'none';
    reactionTestWaiting.style.display = 'none';
    reactionTestSignaled.style.display = 'none';
    reactionTestResult.style.display = 'none';
  }

  function showReactionTestSection(section) {
    hideAllReactionTestSections();
    section.style.display = 'block';
  }

  async function waitForFlapHit(nodeId, timeoutMs = 10000) {
    const startTime = Date.now();

    return new Promise((resolve) => {
      const checkInterval = setInterval(async () => {
        const elapsed = Date.now() - startTime;
        
        if (elapsed > timeoutMs) {
          clearInterval(checkInterval);
          resolve(null); // Timeout
          return;
        }

        const status = await checkFlapStatus(nodeId);
        if (!status) {
          clearInterval(checkInterval);
          resolve(null); // Flap gone
          return;
        }

        // Check if flap is down - lastUpTimeMs is already the measured reaction time
        const isFlapDown = (Number(status.flags) & 0x01) === 0;
        
        if (isFlapDown) {
          clearInterval(checkInterval);
          const reactionTimeMs = Number(status.lastUpTimeMs);
          resolve(reactionTimeMs > 0 ? reactionTimeMs : null);
        }
      }, 50); // Poll every 50ms
    });
  }

  async function playReactionTestRound() {
    // Find random down flap
    const flap = await findRandomDownFlap();
    if (!flap) {
      showReactionTestSection(reactionTestInitial);
      reactionTestSession.running = false;
      alert('Keine Klappe verfügbar!');
      return;
    }

    reactionTestSession.currentTargetNodeId = flap.id;
    
    // Send RESET command
    try {
      await postFlapAction('node_' + flap.id, 'reset');
    } catch (err) {
      console.error('Reset command failed:', err);
      showReactionTestSection(reactionTestInitial);
      reactionTestSession.running = false;
      return;
    }

    // Show signal section
    showReactionTestSection(reactionTestSignaled);

    // Wait for hit (max 10 seconds)
    const reactionTimeMs = await waitForFlapHit(flap.id, 10000);

    if (reactionTimeMs === null) {
      // Timeout or error
      console.log('Timeout waiting for hit');
      showReactionTestSection(reactionTestInitial);
      reactionTestSession.running = false;
      return;
    }

    // Hit detected! Record reaction time
    reactionTestSession.reactionTimes.push(reactionTimeMs);
    reactionTestSession.hitsCollected++;

    // Show result for this hit
    document.getElementById('reactionTimeDisplay').textContent = reactionTimeMs + ' ms';
    document.getElementById('reactionTestStats').innerHTML = 
      '<p>Treffer ' + reactionTestSession.hitsCollected + ' / ' + reactionTestSession.hitsTarget + '</p>';

    showReactionTestSection(reactionTestResult);

    if (reactionTestSession.hitsCollected >= reactionTestSession.hitsTarget) {
      // Test complete
      reactionTestSession.running = false;
      
      // Calculate average
      const average = Math.round(
        reactionTestSession.reactionTimes.reduce((a, b) => a + b, 0) / reactionTestSession.reactionTimes.length
      );
      
      // Save best time if better
      saveBestTime(reactionTestSession.reactionTimes);
      
      const roundTexts = reactionTestSession.reactionTimes
        .map((time, idx) => 'Treffer ' + (idx + 1) + ': ' + time + ' ms')
        .join('\n');

      document.getElementById('reactionTestStats').innerHTML = 
        '<p>Durchschnitt: ' + average + ' ms</p>' +
        '<p>Treffer gesammt: ' + reactionTestSession.hitsCollected + '</p>' +
        '<p style="margin-top: 10px; white-space: pre-wrap;">' + roundTexts + '</p>';

      return; // Keep showing result, user clicks again/close
    }

    // Continue to next round after delay
    setTimeout(() => {
      if (reactionTestSession.running) {
        playReactionTestRound();
      }
    }, 500);
  }

  async function startReactionTest() {
    try {
      reactionTestSession.running = true;
      reactionTestSession.hitsCollected = 0;
      reactionTestSession.reactionTimes = [];
      
      showReactionTestSection(reactionTestWaiting);
      
      // Start first round
      await playReactionTestRound();
    } catch (err) {
      console.error('Test start error:', err);
      showReactionTestSection(reactionTestInitial);
      reactionTestSession.running = false;
    }
  }

  reactionTestBtn?.addEventListener('click', () => {
    loadAndDisplayBestTime();
    reactionTestModal.style.display = 'block';
    showReactionTestSection(reactionTestInitial);
    reactionTestSession.running = false;
  });

  reactionTestStartBtn?.addEventListener('click', () => {
    startReactionTest();
  });

  reactionTestAgainBtn?.addEventListener('click', () => {
    startReactionTest();
  });

  reactionTestCloseBtns.forEach((btn) => {
    btn?.addEventListener('click', () => {
      reactionTestModal.style.display = 'none';
      reactionTestSession.running = false;
    });
  });

  // Close modal when clicking outside
  window.addEventListener('click', (e) => {
    if (e.target === reactionTestModal) {
      reactionTestModal.style.display = 'none';
      reactionTestSession.running = false;
    }
  });

  // Load best time on page load
  loadAndDisplayBestTime();
});
