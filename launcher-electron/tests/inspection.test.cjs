const { test } = require('node:test');
const assert = require('node:assert/strict');
test('validates persistent worker inspection and removes unrelated fields', () => {
  const { parseInspection } = require('../dist/inspection.js');
  const valid = { status: 'ready', root: '/synthetic', game_dir: '/synthetic/game', job_marker_present: false,
    game_running: null, hide_while_playing: true, secret: 'private' };
  assert.equal(parseInspection(valid).status, 'ready');
  assert.equal('secret' in parseInspection(valid), false);
  assert.throws(() => parseInspection({ ...valid, game_running: false }));
  assert.throws(() => parseInspection({ ...valid, status: 'invented' }));
});

test('exposes only the safe interrupted-build state to the renderer', () => {
  const { parseInspection } = require('../dist/inspection.js');
  const valid = { status: 'ready', root: '/synthetic', game_dir: '/synthetic/game', job_marker_present: true,
    game_running: null, hide_while_playing: true };
  const recovery = { state: 'alive', kind: 'rebuild', reason: 'job_object', pid: 123 };
  assert.deepEqual(parseInspection({ ...valid, recovery }).recovery, { state: 'alive', kind: 'rebuild' });
  assert.throws(() => parseInspection({ ...valid, recovery: { ...recovery, state: 'invented' } }));
});
