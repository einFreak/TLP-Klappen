import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const { LineDecoder, SerialTransport } = createRequire(import.meta.url)('../serial_transport.js');
const encode = (text) => new TextEncoder().encode(text);

class FakePort {
  constructor(options = {}) {
    this.options = options;
    this.commands = [];
    this.session = 12345;
    this.readable = new ReadableStream({ start: (controller) => { this.controller = controller; } });
    this.writable = new WritableStream({ write: (chunk) => this.receive(new TextDecoder().decode(chunk)) });
  }
  async open(options) { this.openOptions = options; }
  async close() { this.closed = true; }
  frame(id, type, data, session = this.session) {
    const bytes = encode('TLP1 ' + JSON.stringify({ id, session, type, data }) + '\r\n');
    this.controller.enqueue(bytes.slice(0, 9));
    this.controller.enqueue(bytes.slice(9));
  }
  snapshot() {
    this.frame(0, 'snapshot_begin', { generation: 1 });
    this.frame(0, 'central', { mode: 'local', canNormal: 1 });
    this.frame(0, 'node', { id: 3, online: 1, flags: 1, lastUpEventCounter: 4 });
    this.frame(0, 'snapshot_end', { generation: 1 });
  }
  receive(line) {
    const [prefix, rawId, operation, session, ...args] = line.trim().split(' ');
    assert.equal(prefix, 'TLP1');
    const id = Number(rawId);
    this.commands.push({ id, operation, session: Number(session), args });
    if (this.options.silent) return;
    if (operation === 'HELLO') {
      this.controller.enqueue(encode('Boot diagnostic\n'));
      this.frame(id, 'hello', { device: this.options.device || 'TLP-NodeMCU', version: 1, mode: 'wifi' });
    } else if (operation === 'ENTER_LOCAL') {
      this.frame(id, 'mode', { mode: 'local' });
      if (!this.options.noSnapshot) this.snapshot();
    } else if (operation === 'EXIT_LOCAL') this.frame(id, 'mode', { mode: 'wifi' });
    else if (operation === 'PING' && !this.options.noPing) this.frame(id, 'pong', { ok: 1 });
    else if (operation === 'FLAP' || operation === 'THRESHOLD') {
      if (this.options.reject) this.frame(id, 'error', { error: 'pending queue full' });
      else this.frame(id, 'accepted', { pending: 1, requestId: 8 });
    } else if (operation === 'RESULT') this.frame(id, 'result', { requestId: 8, done: 1, state: 'ack' });
  }
}

function transport(options = {}) {
  return new SerialTransport(null, { timeoutMs: 50, handshakeMs: 100, snapshotMs: 100, pingIntervalMs: 10000, ...options });
}

test('line framing handles split input, CRLF, boot text and overflow recovery', () => {
  const decoder = new LineDecoder(16);
  assert.deepEqual(decoder.push(encode('boot\r\nTLP')), ['boot']);
  assert.deepEqual(decoder.push(encode('1 {}\n')), ['TLP1 {}']);
  assert.deepEqual(decoder.push(encode('x'.repeat(20) + '\nOK\n')), ['OK']);
});

test('handshake, atomic snapshot, delta cache, command/result, explicit exit and lock release', async () => {
  const client = transport();
  const port = new FakePort();
  const statuses = [];
  client.subscribe((status) => statuses.push(status), () => {});
  await client.connect(port);
  try {
    assert.equal(port.openOptions.baudRate, 115200);
    assert.deepEqual(port.commands.slice(0, 2).map((command) => command.operation), ['HELLO', 'ENTER_LOCAL']);
    assert.equal(port.commands[0].session, 0);
    assert.equal(port.commands[1].session, port.session);
    assert.equal((await client.getStatus()).nodes[0].id, 3);
    assert.equal(statuses[0].nodes.length, 1);
    assert.equal((await client.flap(3, 'up')).requestId, 8);
    assert.equal((await client.threshold(3, 1023)).pending, 1);
    assert.equal((await client.result(8)).state, 'ack');
    port.frame(0, 'node', { id: 3, online: 0, lastUpEventCounter: 5 });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal((await client.getStatus()).nodes[0].online, 0);
    assert.equal(port.commands.filter((command) => command.operation === 'STATUS').length, 0);
    port.frame(0, 'node', { id: 3, online: 1 }, 999);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal((await client.getStatus()).nodes[0].online, 0);
  } finally { await client.disconnect(); }
  assert.equal(port.commands.at(-1).operation, 'EXIT_LOCAL');
  assert.ok(port.closed);
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
  await assert.rejects(client.getStatus(), /nicht verbunden/);
  await assert.rejects(client.flap(3, 'up'), /nicht verbunden/);
});

test('wrong device never receives ENTER_LOCAL', async () => {
  const client = transport();
  const port = new FakePort({ device: 'other' });
  await assert.rejects(client.connect(port), /Falsches Geraet/);
  assert.deepEqual(port.commands.map((command) => command.operation), ['HELLO']);
  assert.ok(port.closed);
});

test('missing snapshot and silent device time out and close the port', async () => {
  for (const options of [{ noSnapshot: true }, { silent: true }]) {
    const client = transport();
    const port = new FakePort(options);
    await assert.rejects(client.connect(port), /unvollstaendig|gefunden/);
    assert.ok(port.closed);
    assert.equal(client.ready, false);
  }
});

test('invalid arguments never write; controller rejects are surfaced without command replay', async () => {
  const client = transport();
  const port = new FakePort({ reject: true });
  await client.connect(port);
  try {
    const before = port.commands.length;
    await assert.rejects(client.flap(32, 'up'), /Ungueltig/);
    await assert.rejects(client.flap(3, 'up\nRESET'), /Ungueltig/);
    await assert.rejects(client.threshold(3, 1024), /Ungueltig/);
    assert.equal(port.commands.length, before);
    await assert.rejects(client.flap(3, 'up'), /queue full/);
    assert.equal(port.commands.filter((command) => command.operation === 'FLAP').length, 1);
  } finally { await client.disconnect(); }
});

test('read failure cancels pending calls and invalidates cached status', async () => {
  const client = transport();
  const port = new FakePort();
  await client.connect(port);
  const pending = client.request('PING');
  const rejected = assert.rejects(pending, /getrennt/);
  port.controller.error(new Error('USB removed'));
  await rejected;
  await client.disconnect(false);
  assert.equal(client.ready, false);
  assert.ok(port.closed);
});

test('lost PING response closes connection without replay or implicit re-entry', async () => {
  const client = transport({ timeoutMs: 25, pingIntervalMs: 10 });
  const port = new FakePort({ noPing: true });
  await client.connect(port);
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('heartbeat did not disconnect')), 1000);
    client.subscribe(() => {}, (connected) => {
      if (!connected && !client.port) { clearTimeout(timer); resolve(); }
    });
  });
  assert.equal(client.ready, false);
  assert.ok(port.closed);
  assert.equal(port.commands.filter((command) => command.operation === 'ENTER_LOCAL').length, 1);
});

test('parallel disconnects send EXIT_LOCAL only once', async () => {
  const client = transport();
  const port = new FakePort();
  await client.connect(port);
  await Promise.all([client.disconnect(), client.disconnect()]);
  assert.equal(port.commands.filter((command) => command.operation === 'EXIT_LOCAL').length, 1);
  assert.ok(port.closed);
});