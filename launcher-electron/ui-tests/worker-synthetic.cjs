const { _electron } = require('playwright-core');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');

(async () => {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-worker-ui-'));
  const root = path.join(base, 'nx2saria');
  let app;
  try {
    const game = path.join(root, 'game');
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game, hide_while_playing: true }));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: 'test', steps: { compile: {} } }));
    await fs.writeFile(path.join(game, 'ocarina'), '#!/bin/sh\nsleep 2\n', { mode: 0o755 });
    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'), env: { ...process.env, XDG_DATA_HOME: base } });
    await app.evaluate(({ dialog }, selected) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] }); }, root);
    await app.evaluate(({ dialog }) => { globalThis.workerWarnings = []; dialog.showMessageBox = async (_window, options) => { globalThis.workerWarnings.push(options.message); return { response: 0 }; }; });
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 10000 });
    for (let round = 0; round < 2; round++) {
      await page.getByRole('button', { name: 'Jogar' }).click();
      await page.waitForFunction(() => document.querySelector('#status').textContent.includes('Abrindo'));
      let hidden = false;
      for (let tries = 0; tries < 30; tries++) {
        if (!await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible())) { hidden = true; break; }
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      assert.equal(hidden, true);
      if (round === 0) {
        const staged = path.join(root, 'app', '999.0.0');
        await fs.mkdir(path.join(staged, 'launcher'), { recursive: true });
        await fs.writeFile(path.join(staged, 'launcher', 'start.pyw'), 'synthetic');
        await fs.writeFile(path.join(staged, 'VERSION'), '999.0.0');
        await fs.writeFile(path.join(staged, 'staged.json'), JSON.stringify({ version: '999.0.0', runtime: '' }));
      }
      await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 10000 });
      if (round === 0) assert.ok((await app.evaluate(() => globalThis.workerWarnings)).some(message => message.includes('atualização')));
      assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), true);
      await page.waitForFunction(() => !document.querySelector('#play').disabled);
    }

    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 10000 });
    await app.evaluate(({ dialog }) => { dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] }); });
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.getByText('Nenhuma instalação selecionada').waitFor({ timeout: 10000 });
    assert.equal((await page.evaluate(() => window.launcher.play())).error, 'unavailable');
    await app.evaluate(({ dialog }, selected) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] }); }, root);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 10000 });
    await fs.writeFile(path.join(root, 'data', 'job-running.json'), '{}');
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.getByText('Há um registro de trabalho:', { exact: false }).waitFor({ timeout: 10000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher.log'), 'utf8');
    assert.match(log, /worker: started/);
    assert.equal((log.match(/worker: game ended/g) || []).length, 2);
    await fs.rm(path.join(root, 'data', 'job-running.json'));
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await fs.writeFile(path.join(game, 'ocarina'), '#!/bin/sh\nsleep 2\nprintf done > survived-worker\n', { mode: 0o755 });
    await page.getByRole('button', { name: 'Jogar' }).click();
    let workerPid = 0;
    for (let tries = 0; tries < 30 && !workerPid; tries++) {
      for (const pid of await fs.readdir('/proc')) {
        if (!/^\d+$/.test(pid)) continue;
        const cmd = await fs.readFile(`/proc/${pid}/cmdline`, 'utf8').catch(() => '');
        if (cmd.includes('launcher.worker') && cmd.includes(root)) { workerPid = Number(pid); break; }
      }
      if (!workerPid) await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.notEqual(workerPid, 0);
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('Abrindo'));
    for (let tries = 0; tries < 30; tries++) {
      if (!await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible())) break;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    process.kill(workerPid, 'SIGKILL');
    await page.getByText('Não foi possível iniciar').waitFor({ timeout: 10000 });
    assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), true);
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    let survived = false;
    for (let tries = 0; tries < 60; tries++) {
      if (await fs.readFile(path.join(game, 'survived-worker'), 'utf8').catch(() => '') === 'done') { survived = true; break; }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.equal(survived, true);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await fs.writeFile(path.join(game, 'ocarina'), '#!/bin/sh\nsleep 2\nprintf done > game-finished\n', { mode: 0o755 });
    await page.getByRole('button', { name: 'Jogar' }).click();
    let started = false;
    for (let tries = 0; tries < 30; tries++) {
      if (!await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible())) { started = true; break; }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.equal(started, true);
    await app.close();
    app = null;
    let completed = false;
    for (let tries = 0; tries < 100; tries++) {
      const marker = await fs.readFile(path.join(game, 'game-finished'), 'utf8').catch(() => '');
      const currentLog = await fs.readFile(path.join(root, 'data', 'logs', 'launcher.log'), 'utf8');
      if (marker === 'done' && (currentLog.match(/worker: game ended/g) || []).length === 3) { completed = true; break; }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.equal(completed, true);
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
