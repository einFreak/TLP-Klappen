(function (root) {
  'use strict';

  class LineDecoder {
    constructor(limit = 512) {
      this.limit = limit;
      this.decoder = new TextDecoder();
      this.line = '';
      this.overflow = false;
    }

    push(chunk) {
      const lines = [];
      for (const character of this.decoder.decode(chunk, { stream: true })) {
        if (character === '\n') {
          if (!this.overflow) lines.push(this.line.replace(/\r$/, ''));
          this.line = '';
          this.overflow = false;
        } else if (!this.overflow) {
          this.line += character;
          if (this.line.length >= this.limit) {
            this.line = '';
            this.overflow = true;
          }
        }
      }
      return lines;
    }
  }

  class SerialTransport {
    constructor(serial = root.navigator?.serial, options = {}) {
      this.local = true;
      this.serial = serial;
      this.timeoutMs = options.timeoutMs ?? 2500;
      this.pingIntervalMs = options.pingIntervalMs ?? 2000;
      this.handshakeMs = options.handshakeMs ?? 8000;
      this.snapshotMs = options.snapshotMs ?? 10000;
      this.session = 0;
      this.nextId = 1;
      this.pending = new Map();
      this.subscribers = new Set();
      this.nodes = new Map();
      this.central = {};
      this.staging = null;
      this.ready = false;
      this.connecting = false;
      this.closing = false;
      this.port = null;
      this.writeChain = Promise.resolve();
    }

    subscribe(onStatus, onConnection) {
      const subscriber = { onStatus, onConnection };
      this.subscribers.add(subscriber);
      onConnection(this.ready, this.ready ? 'USB verbunden' : 'USB nicht verbunden');
      return () => this.subscribers.delete(subscriber);
    }

    connection(connected, message) {
      for (const subscriber of this.subscribers) subscriber.onConnection(connected, message);
    }

    publish() {
      if (!this.ready) return;
      const status = this.status();
      for (const subscriber of this.subscribers) subscriber.onStatus(status);
    }

    status() {
      if (!this.ready) throw new Error('USB nicht verbunden');
      return { ...this.central, nodes: Array.from(this.nodes.values()) };
    }

    async getStatus() { return this.status(); }

    async flap(node, action) {
      if (!Number.isInteger(node) || node < 0 || node > 31 || !['up', 'down', 'toggle', 'auto', 'reset'].includes(action)) {
        throw new Error('Ungueltiger Klappenbefehl');
      }
      return (await this.request('FLAP', [node, action])).data;
    }

    async threshold(node, value) {
      if (!Number.isInteger(node) || node < 0 || node > 31 || !Number.isInteger(value) || value < 0 || value > 1023) {
        throw new Error('Ungueltiger Schwellwert');
      }
      return (await this.request('THRESHOLD', [node, value])).data;
    }

    async result(id) {
      if (!Number.isInteger(id) || id < 1 || id > 255) throw new Error('Ungueltige CAN-Anfrage');
      return (await this.request('RESULT', [id])).data;
    }

    request(operation, args = [], timeoutMs = this.timeoutMs) {
      if (!this.writer || this.closing) return Promise.reject(new Error('USB nicht verbunden'));
      if (!['HELLO', 'ENTER_LOCAL', 'PING', 'EXIT_LOCAL'].includes(operation) && !this.ready) {
        return Promise.reject(new Error('USB noch nicht bereit'));
      }
      const id = this.nextId++;
      if (id > 2147483647) return Promise.reject(new Error('Sitzungslimit erreicht; bitte neu verbinden'));
      const command = ['TLP1', id, operation, operation === 'HELLO' ? 0 : this.session, ...args].join(' ') + '\n';
      if (command.length > 128 || /[\r\n]/.test(command.slice(0, -1))) {
        return Promise.reject(new Error('Ungueltige Protokollzeile'));
      }
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.pending.delete(id);
          reject(new Error('USB-Antwortzeit ueberschritten: ' + operation));
        }, timeoutMs);
        this.pending.set(id, { resolve, reject, timer, operation });
        this.writeChain = this.writeChain.catch(() => {}).then(async () => {
          if (!this.writer || !this.pending.has(id) || this.closing) throw new Error('USB getrennt');
          await this.writer.write(new TextEncoder().encode(command));
        });
        this.writeChain.catch((error) => {
          const pending = this.pending.get(id);
          if (!pending) return;
          clearTimeout(timer);
          this.pending.delete(id);
          reject(error);
        });
      });
    }

    handleFrame(line) {
      if (!line.startsWith('TLP1 ')) return;
      let frame;
      try { frame = JSON.parse(line.slice(5)); } catch (_) { return; }
      if (!Number.isInteger(frame.id) || frame.id < 0 || !Number.isInteger(frame.session) || frame.session < 1 ||
          typeof frame.type !== 'string' || !frame.data || typeof frame.data !== 'object') return;
      if (frame.id !== 0) {
        const pending = this.pending.get(frame.id);
        if (!pending || (pending.operation !== 'HELLO' && frame.session !== this.session)) return;
        const expected = { HELLO: 'hello', ENTER_LOCAL: 'mode', EXIT_LOCAL: 'mode', PING: 'pong',
          FLAP: 'accepted', THRESHOLD: 'accepted', RESULT: 'result', STATUS: 'ok' };
        if (frame.type !== 'error' && frame.type !== expected[pending.operation]) return;
        clearTimeout(pending.timer);
        this.pending.delete(frame.id);
        if (frame.type === 'error') pending.reject(new Error(frame.data.error || 'Controller-Fehler'));
        else pending.resolve(frame);
        return;
      }
      if (frame.session !== this.session || !this.session) return;
      const data = frame.data;
      if (frame.type === 'snapshot_begin' && Number.isInteger(data.generation)) {
        this.staging = { generation: data.generation, central: null, nodes: new Map() };
      } else if (frame.type === 'central') {
        if (data.mode !== 'local') return;
        if (this.staging) this.staging.central = data;
        else this.central = data;
      } else if (frame.type === 'node' && Number.isInteger(data.id) && data.id >= 0 && data.id <= 31) {
        (this.staging ? this.staging.nodes : this.nodes).set(data.id, data);
      } else if (frame.type === 'snapshot_end' && this.staging && data.generation === this.staging.generation && this.staging.central) {
        this.nodes = this.staging.nodes;
        this.central = this.staging.central;
        this.staging = null;
        this.ready = true;
        this.snapshotResolve?.();
        this.connection(true, 'USB verbunden; WLAN aus');
      }
      this.publish();
    }

    async readLoop(reader) {
      const decoder = new LineDecoder();
      try {
        while (!this.closing) {
          const { value, done } = await reader.read();
          if (done) break;
          for (const line of decoder.push(value)) this.handleFrame(line);
        }
        if (!this.closing) this.fail(new Error('USB-Verbindung beendet'));
      } catch (error) {
        if (!this.closing) this.fail(error);
      } finally {
        reader.releaseLock();
      }
    }

    async connect(port) {
      if (this.port || this.connecting || this.closing) throw new Error('USB-Port bereits geoeffnet');
      if (!port && !this.serial) throw new Error('Web Serial benoetigt Edge oder Chrome auf localhost');
      this.connecting = true;
      this.connection(false, 'USB verbinden...');
      try {
        this.port = port || await this.serial.requestPort();
        await this.port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
        this.session = 0;
        this.nextId = 1;
        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.readTask = this.readLoop(this.reader);
        const deadline = Date.now() + this.handshakeMs;
        let hello;
        while (Date.now() < deadline) {
          try { hello = await this.request('HELLO', [], Math.min(this.timeoutMs, 1000)); break; }
          catch (error) {
            if (this.closing || !this.writer || !error.message.startsWith('USB-Antwortzeit')) throw error;
          }
        }
        if (!hello) throw new Error('Kein TLP-Controller gefunden');
        if (hello.data.device !== 'TLP-NodeMCU' || hello.data.version !== 1 || hello.data.mode !== 'wifi') {
          throw new Error('Falsches Geraet oder inkompatible Firmware');
        }
        this.session = hello.session;
        const mode = await this.request('ENTER_LOCAL');
        if (mode.data.mode !== 'local') throw new Error('Lokaler Modus nicht bestaetigt');
        this.pingTimer = setInterval(() => {
          if (this.pinging) return;
          this.pinging = true;
          this.request('PING').catch((error) => this.fail(error)).finally(() => { this.pinging = false; });
        }, this.pingIntervalMs);
        if (!this.ready) {
          await new Promise((resolve, reject) => {
            this.snapshotResolve = resolve;
            this.snapshotReject = reject;
            this.snapshotTimer = setTimeout(() => reject(new Error('USB-Status unvollstaendig')), this.snapshotMs);
          });
        }
        clearTimeout(this.snapshotTimer);
        this.snapshotResolve = this.snapshotReject = null;
      } catch (error) {
        await this.disconnect(false);
        this.connection(false, error.message);
        throw error;
      } finally {
        this.connecting = false;
      }
    }

    fail(error) {
      this.ready = false;
      this.connection(false, error.message);
      void this.disconnect(false).catch(() => {});
    }

    async disconnect(graceful = true) {
      if (this.closeTask) return this.closeTask;
      this.closeTask = this.close(graceful);
      try { return await this.closeTask; }
      finally { this.closeTask = null; }
    }

    async close(graceful) {
      clearInterval(this.pingTimer);
      let exitError = null;
      if (graceful && this.session && this.writer) {
        try {
          const mode = await this.request('EXIT_LOCAL');
          if (mode.data.mode !== 'wifi') throw new Error('WLAN-Neustart fehlgeschlagen');
        } catch (error) { exitError = error; }
      }
      this.closing = true;
      this.ready = false;
      this.snapshotReject?.(new Error('USB getrennt'));
      clearTimeout(this.snapshotTimer);
      this.snapshotResolve = this.snapshotReject = null;
      for (const pending of this.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(new Error('USB getrennt'));
      }
      this.pending.clear();
      try {
        if (this.reader) await this.reader.cancel().catch(() => {});
        if (this.readTask) await this.readTask;
        if (this.writer) {
          await this.writer.abort().catch(() => {});
          this.writer.releaseLock();
        }
        if (this.port) await this.port.close().catch(() => {});
      } finally {
        this.reader = this.writer = this.port = null;
        this.readTask = null;
        this.session = 0;
        this.staging = null;
        this.nodes.clear();
        this.central = {};
        this.closing = false;
        this.connection(false, exitError ? 'USB getrennt; WLAN-Rueckfall nach Timeout' : 'USB nicht verbunden');
      }
      if (exitError) throw exitError;
    }
  }

  if (typeof module !== 'undefined' && module.exports) module.exports = { LineDecoder, SerialTransport };
  if (typeof document !== 'undefined') root.TLPTransport = new SerialTransport();
})(globalThis);