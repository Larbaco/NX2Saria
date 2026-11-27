const { _electron } = require('playwright-core');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');

(async () => {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-mods-ui-'));
  const root = path.join(base, 'nx2saria');
  let app;
  try {
    const game = path.join(root, 'game');
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ auto_check: false, game_dir: game, hide_while_playing: false }));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: 'test', steps: { compile: {} } }));
    await fs.writeFile(path.join(game, 'ocarina'), '#!/bin/sh\nsleep 1\n', { mode: 0o755 });
    const python = path.join(base, 'worker-python');
    await fs.writeFile(python, `#!/usr/bin/env python3
import os,runpy,sys,time
sys.path.insert(0,os.getcwd())
from unittest import mock
from launcher import jobs,net,mods,build
class Fake(jobs.Job):
    claims=('game',); kind='rebuild'
    def __init__(self,ctx,game):
        super().__init__(ctx,['compile']); self.game_dir=game; self.after='play'
    def run(self):
        self.begin('compile')
        for i in range(80):
            if self.cancelled(): raise net.Cancelled()
            time.sleep(.025)
sys.argv=['launcher.worker','--root',sys.argv[-1]]
with mock.patch.object(jobs,'ModsRebuildJob',Fake), mock.patch.object(mods,'prepare',return_value=[]), mock.patch.object(build,'can_rebuild',return_value=True), mock.patch.object(mods,'code_waiting',return_value=['Example']):
    runpy.run_module('launcher.worker',run_name='__main__')
`, { mode: 0o755 });
    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'), env: { ...process.env, XDG_DATA_HOME: base, NX2SARIA_TEST_LOCALE: 'pt-BR', NX2SARIA_TELEMETRY_URL: 'none', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', NX2SARIA_PYTHON: python } });
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 10000 });
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.getByRole('button', { name: 'Cancelar compilação' }).waitFor({ timeout: 10000 });
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 15000 });
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.getByRole('button', { name: 'Cancelar compilação' }).click({ timeout: 10000 });
    await page.locator('#status').getByText('Não foi possível iniciar').waitFor({ timeout: 10000 });
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 10000 });
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher.log'), 'utf8');
    assert.match(log, /worker: job result=ok/);
    assert.match(log, /worker: job result=cancelled/);
    assert.match(log, /whose build did not finish/);
    assert.equal(await fs.stat(path.join(root, 'data', 'job-running.json')).then(() => true).catch(() => false), false);
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
