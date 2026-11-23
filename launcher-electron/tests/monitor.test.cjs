const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createMonitor } = require('../dist/monitor.js');

test('hides only after confirmation and only when the preference is enabled', async () => {
  let state = { game_running: null, hide_while_playing: true };
  let hidden = 0;
  const monitor = createMonitor(async () => state, () => { hidden++; return true; });
  await monitor.check();
  state = { game_running: true, hide_while_playing: false };
  await monitor.check();
  assert.equal(hidden, 0);
  state = { game_running: true, hide_while_playing: true };
  await monitor.check();
  await monitor.check();
  assert.equal(hidden, 1);
});

test('ignores a response that arrives after the session ends', async () => {
  let finish;
  let hidden = 0;
  const monitor = createMonitor(() => new Promise(resolve => { finish = resolve; }), () => { hidden++; return true; });
  const pending = monitor.check();
  monitor.stop();
  finish({ game_running: true, hide_while_playing: true });
  await pending;
  assert.equal(hidden, 0);
});
