(function () {
  async function json(url, options) {
    const response = await fetch(url, options);
    if (!response.ok) {
      const body = await response.json().catch(() => ({}));
      throw new Error(body.error || 'HTTP ' + response.status);
    }
    return response.json();
  }

  window.TLPTransport = {
    local: false,
    getStatus: () => json('/api/status'),
    flap: (node, action) => json('/api/flap?action=' + encodeURIComponent(action) + '&node=' + encodeURIComponent(node), { method: 'POST' }),
    threshold: (node, value) => json('/api/threshold?value=' + encodeURIComponent(value) + '&node=' + encodeURIComponent(node), { method: 'POST' }),
    result: (id) => json('/api/command-result?id=' + encodeURIComponent(id)),
    subscribe(onStatus, onConnection) {
      let source;
      let retry;
      let backoff = 1000;
      let closed = false;
      function connect() {
        if (closed) return;
        source = new EventSource('/events');
        source.addEventListener('status', (event) => {
          try {
            const status = JSON.parse(event.data);
            backoff = 1000;
            onConnection(true, 'Live');
            onStatus(status);
          } catch (_) {}
        });
        source.onerror = () => {
          onConnection(false, 'WLAN-Verbindung unterbrochen');
          source.close();
          retry = setTimeout(connect, backoff);
          backoff = Math.min(backoff * 2, 30000);
        };
      }
      connect();
      return () => { closed = true; clearTimeout(retry); source?.close(); };
    },
  };
})();