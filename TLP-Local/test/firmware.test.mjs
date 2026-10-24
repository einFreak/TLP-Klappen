import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

test('actual firmware parser rejects malformed arguments and integer overflows', async (context) => {
  const compiler = process.env.CXX || 'g++';
  if (spawnSync(compiler, ['--version']).error) { context.skip('Set CXX to a host C++ compiler'); return; }
  const directory = await mkdtemp(join(tmpdir(), 'tlp-protocol-'));
  context.after(() => rm(directory, { recursive: true, force: true }));
  const binary = join(directory, process.platform === 'win32' ? 'protocol.exe' : 'protocol');
  const source = fileURLToPath(new URL('./serial_protocol_test.cpp', import.meta.url));
  const includes = fileURLToPath(new URL('../../TLP-NodeMCU/include', import.meta.url));
  const build = spawnSync(compiler, ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', includes, source, '-o', binary], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr || build.error?.message);
  const run = spawnSync(binary, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr || run.error?.message);
});