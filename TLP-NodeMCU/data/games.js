// ============================================================
// TLP-Klappen Game Modes
// ============================================================

(function () {
  const LS_BEST_TIME_KEY = 'tlp_reaction_best_time';

  async function findRandomDownFlap() {
    try {
      const status = await window.TLPTransport.getStatus();
      const nodes = status.nodes || [];
      const downFlaps = nodes.filter((n) => {
        const isOnline = Number(n.online) === 1;
        const isFlapDown = (Number(n.flags) & 0x01) === 0;
        return isOnline && isFlapDown;
      });

      if (downFlaps.length === 0) return null;
      return downFlaps[Math.floor(Math.random() * downFlaps.length)];
    } catch (err) {
      console.error('findRandomDownFlap error:', err);
      return null;
    }
  }

  async function checkFlapStatus(nodeId) {
    try {
      const status = await window.TLPTransport.getStatus();
      const nodes = status.nodes || [];
      const flap = nodes.find((n) => Number(n.id) === Number(nodeId));
      return flap || null;
    } catch (err) {
      console.error('checkFlapStatus error:', err);
      return null;
    }
  }

  function loadAndDisplayBestTime() {
    try {
      const bestTimeStr = localStorage.getItem(LS_BEST_TIME_KEY);
      const bestTime = bestTimeStr ? Number(bestTimeStr) : null;
      const bestTimeEl = document.getElementById('reactionTestBestTime');
      if (!bestTimeEl) return;

      if (bestTime !== null && Number.isFinite(bestTime)) {
        bestTimeEl.textContent = 'Best: ' + bestTime + ' ms';
      } else {
        bestTimeEl.textContent = '';
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

  window.setupReactionGames = function setupReactionGames() {
    const reactionTestBtn = document.getElementById('reactionTestBtn');
    const reactionTestModal = document.getElementById('reactionTestModal');
    const reactionTestInitial = document.getElementById('reactionTestInitial');
    const reactionTestWaiting = document.getElementById('reactionTestWaiting');
    const reactionTestSignaled = document.getElementById('reactionTestSignaled');
    const reactionTestResult = document.getElementById('reactionTestResult');
    const reactionTestStartBtn = document.getElementById('reactionTestStartBtn');
    const reactionTestAgainBtn = document.getElementById('reactionTestAgainBtn');
    const reactionTestCloseBtns = document.querySelectorAll('#reactionTestCloseBtn, #reactionTestCloseBtn2');

    if (!reactionTestBtn || !reactionTestModal) {
      return;
    }

    const reactionTestSession = {
      running: false,
      hitsTarget: 5,
      hitsCollected: 0,
      reactionTimes: [],
      currentTargetNodeId: -1,
    };

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

    async function waitForFlapHit(nodeId, timeoutMs, previousCounter) {
      const startTime = Date.now();

      return new Promise((resolve) => {
        const checkInterval = setInterval(async () => {
          if (!reactionTestSession.running || !state.connected) {
            clearInterval(checkInterval);
            resolve(null);
            return;
          }
          const elapsed = Date.now() - startTime;

          if (elapsed > timeoutMs) {
            clearInterval(checkInterval);
            resolve(null);
            return;
          }

          const status = await checkFlapStatus(nodeId);
          if (!status) {
            clearInterval(checkInterval);
            resolve(null);
            return;
          }

          const isFlapDown = (Number(status.flags) & 0x01) === 0;
          const eventSequence = Number(status.lastUpEventSequence ?? status.lastUpEventCounter);
          if (isFlapDown && eventSequence !== previousCounter) {
            clearInterval(checkInterval);
            const reactionTimeMs = Number(status.lastUpTimeMs);
            resolve(reactionTimeMs > 0 ? reactionTimeMs : null);
          }
        }, 50);
      });
    }

    async function playReactionTestRound() {
      const flap = await findRandomDownFlap();
      if (!flap) {
        showReactionTestSection(reactionTestInitial);
        reactionTestSession.running = false;
        alert('Keine Klappe verfuegbar!');
        return;
      }

      reactionTestSession.currentTargetNodeId = flap.id;

      try {
        await postFlapAction('node_' + flap.id, 'reset');
      } catch (err) {
        console.error('Reset command failed:', err);
        showReactionTestSection(reactionTestInitial);
        reactionTestSession.running = false;
        return;
      }

      showReactionTestSection(reactionTestSignaled);

      const reactionTimeMs = await waitForFlapHit(flap.id, 10000, Number(flap.lastUpEventSequence ?? flap.lastUpEventCounter));
      if (reactionTimeMs === null) {
        showReactionTestSection(reactionTestInitial);
        reactionTestSession.running = false;
        return;
      }

      reactionTestSession.reactionTimes.push(reactionTimeMs);
      reactionTestSession.hitsCollected++;

      document.getElementById('reactionTimeDisplay').textContent = reactionTimeMs + ' ms';
      document.getElementById('reactionTestStats').innerHTML =
        '<p>Treffer ' + reactionTestSession.hitsCollected + ' / ' + reactionTestSession.hitsTarget + '</p>';

      showReactionTestSection(reactionTestResult);

      if (reactionTestSession.hitsCollected >= reactionTestSession.hitsTarget) {
        reactionTestSession.running = false;

        const average = Math.round(
          reactionTestSession.reactionTimes.reduce((a, b) => a + b, 0) / reactionTestSession.reactionTimes.length
        );

        saveBestTime(reactionTestSession.reactionTimes);

        const roundTexts = reactionTestSession.reactionTimes
          .map((time, idx) => 'Treffer ' + (idx + 1) + ': ' + time + ' ms')
          .join('\n');

        document.getElementById('reactionTestStats').innerHTML =
          '<p>Durchschnitt: ' + average + ' ms</p>' +
          '<p>Treffer gesamt: ' + reactionTestSession.hitsCollected + '</p>' +
          '<p style="margin-top: 10px; white-space: pre-wrap;">' + roundTexts + '</p>';
        return;
      }

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
        await playReactionTestRound();
      } catch (err) {
        console.error('Test start error:', err);
        showReactionTestSection(reactionTestInitial);
        reactionTestSession.running = false;
      }
    }

    reactionTestBtn.addEventListener('click', () => {
      loadAndDisplayBestTime();
      reactionTestModal.style.display = 'flex';
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

    window.addEventListener('click', (e) => {
      if (e.target === reactionTestModal) {
        reactionTestModal.style.display = 'none';
        reactionTestSession.running = false;
      }
    });

    loadAndDisplayBestTime();
  };
})();
