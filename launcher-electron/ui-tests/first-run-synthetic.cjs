const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const http = require('node:http');
const path = require('node:path');
const { execFileSync, spawn } = require('node:child_process');
const { once } = require('node:events');
const { randomUUID } = require('node:crypto');
const readline = require('node:readline');
const { _electron } = require('playwright-core');

const standInInstall = `
import os
import runpy
import sys
import time
from pathlib import Path
from unittest import mock
from launcher import feed, gamefiles, jobs, net, paths, winsys
from launcher.platform import linux
from launcher.platform import game_executable

def install(job):
    job.begin('check')
    release = Path(job.game_dir).parent / 'allow-install'
    until = time.monotonic() + 20
    while not release.exists():
        if job.cancelled() or time.monotonic() > until:
            raise net.Cancelled()
        time.sleep(0.05)
    cache = Path(job.game_dir, 'build-cache')
    cache.mkdir(parents=True)
    (cache / 'state.json').write_text('{"steps":{"compile":{}},"kit":"0.0.1"}')
    game_executable(job.game_dir).write_text('stand-in that cannot be run')

def rebuild(job):
    job.begin('compile_changed')
    Path(job.game_dir, 'rebuilt').touch()

sys.argv = sys.argv[2:]
base = Path(sys.argv[-1]).parent
def search_dirs(extra):
    until = time.monotonic() + 10
    while not (base / 'allow-search').exists() and time.monotonic() < until:
        time.sleep(0.05)
    return [base / 'Downloads']

with (mock.patch.object(jobs.InstallJob, 'run', install),
      mock.patch.object(feed, 'load', return_value={'type': 'url', 'url': os.environ['NX2SARIA_TEST_FEED']}),
      mock.patch.object(jobs.RebuildJob, 'run', rebuild),
      mock.patch.object(winsys, 'reveal'),
      mock.patch.object(winsys, 'open_path'),
      mock.patch.object(paths, 'free_gb', return_value=100),
      mock.patch.object(linux, 'mounted_drives', return_value=[]),
      mock.patch.object(linux, 'toolchain_missing', return_value='Missing synthetic header: opus/opus.h'),
      mock.patch.object(gamefiles, 'candidate_dirs', search_dirs)):
    runpy.run_module('launcher.worker', run_name='__main__')
`;

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-first-run-ui-'));
  const root = path.join(base, 'nx2saria');
  const settings = path.join(root, 'data', 'launcher.json');
  const folder = path.join(base, 'games');
  let app;
  const previousBuilds = [];
  const feedRequests = [];
  const server = http.createServer((request, response) => {
    feedRequests.push(request.url);
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify({ format: 1, version: '999.0.0', app: { name: 'stand-in.zip' },
      notes: '- A synthetic update note.' }));
  });
  try {
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(settings, '{"desktop_shortcut":false}');
    await fs.mkdir(folder);
    await fs.writeFile(path.join(folder, 'unrelated.txt'), 'keep');
    const repository = path.resolve(__dirname, '../..');
    const fixture = path.join(repository, 'tests', 'fixtures', 'worker_install_dump.py');
    const downloads = path.join(base, 'Downloads');
    await fs.mkdir(downloads);
    const found = path.join(downloads, 'found.nup');
    execFileSync('python3', [fixture, found], { cwd: repository, env: { ...process.env, PYTHONPATH: repository } });
    const python = path.join(base, 'test-python');
    await fs.writeFile(python, '#!/bin/sh\nexec "$NX2SARIA_TEST_PYTHON" -c "$NX2SARIA_TEST_INSTALL" "$@"\n', { mode: 0o755 });
    app = await _electron.launch({
      args: ['--no-sandbox', '.'],
      cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, XDG_DATA_HOME: base, NX2SARIA_TEST_LOCALE: 'pt-BR', NX2SARIA_TELEMETRY_URL: 'none', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', NX2SARIA_PYTHON: python,
        NX2SARIA_TEST_PYTHON: process.env.NX2SARIA_PYTHON || 'python3', NX2SARIA_TEST_INSTALL: standInInstall,
        NX2SARIA_TEST_FEED: `http://127.0.0.1:${server.address().port}/manifest.json` }
    });
    const page = await app.firstWindow();
    const location = page.locator('#game-folder-proposal');
    const install = page.getByRole('button', { name: 'Iniciar instalação' });
    const pick = page.getByRole('button', { name: 'Escolher pasta do jogo' });
    await pick.waitFor({ timeout: 10000 });
    await location.getByText(path.join(root, 'game'), { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await install.isDisabled({ timeout: 5000 }), true);
    await page.getByText('100 GB disponíveis. A instalação precisa de cerca de 34 GB.', { exact: true }).waitFor();
    await page.getByText('Missing synthetic header: opus/opus.h', { exact: true }).waitFor({ timeout: 5000 });
    await page.getByText('Procurando o dump do jogo…').waitFor({ timeout: 5000 });

    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, folder);
    await pick.click();
    await location.getByText(path.join(folder, 'NX2Saria'), { exact: true }).waitFor({ timeout: 10000 });

    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await pick.click();
    await page.waitForFunction(() => !document.querySelector('#pick-game-folder').disabled);
    assert.equal(await location.textContent(), path.join(folder, 'NX2Saria'));
    const valid = path.join(base, 'synthetic-game.nup');
    const invalid = path.join(base, 'invalid.nup');
    execFileSync('python3', [fixture, valid],
      { cwd: repository, env: { ...process.env, PYTHONPATH: repository } });
    await fs.writeFile(invalid, 'synthetic invalid metadata');
    const dump = page.locator('#game-dump-proposal');
    await fs.writeFile(path.join(base, 'allow-search'), '');
    await dump.getByText(found, { exact: true }).waitFor({ timeout: 5000 });
    assert.equal(await fs.readFile(settings, 'utf8'), '{"desktop_shortcut":false}');
    const pickDump = page.getByRole('button', { name: 'Escolher dump do jogo' });
    await app.evaluate(({ dialog }, file) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [file] });
    }, valid);
    await pickDump.click({ timeout: 5000 });
    await dump.getByText(valid, { exact: true }).waitFor({ timeout: 5000 });
    await app.evaluate(({ dialog }, file) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [file] });
    }, invalid);
    await pickDump.click();
    await page.getByText('O arquivo selecionado não é um dump válido do jogo.').waitFor({ timeout: 5000 });
    assert.equal(await dump.textContent(), valid);
    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await pickDump.click();
    await page.waitForFunction(() => !document.querySelector('#pick-game-dump').disabled);
    assert.equal(await dump.textContent(), valid);
    assert.deepEqual(await fs.readdir(folder), ['unrelated.txt']);
    assert.equal(await fs.readFile(settings, 'utf8'), '{"desktop_shortcut":false}');
    const channel = page.locator('#first-run').getByLabel('Canal de atualizações');
    const autoCheck = page.getByRole('checkbox', { name: 'Procurar atualizações ao abrir' });
    assert.equal(await channel.inputValue({ timeout: 5000 }), 'stable');
    assert.equal(await autoCheck.isChecked(), true);
    assert.equal(await page.getByRole('checkbox', { name: 'Atalho na área de trabalho' }).isVisible(), false);
    await channel.selectOption('preview');
    await autoCheck.uncheck();
    await page.waitForFunction(() => !document.querySelector('#first-run-auto-check').matches(':disabled'));
    assert.deepEqual(JSON.parse(await fs.readFile(settings, 'utf8')), {
      desktop_shortcut: false, channel: 'preview', auto_check: false,
    });
    await install.click();
    const cancel = page.getByRole('button', { name: 'Cancelar compilação' });
    await cancel.click({ timeout: 5000 });
    await page.getByText('Instalação cancelada', { exact: true }).waitFor({ timeout: 10000 });
    await assert.rejects(fs.access(path.join(root, 'data', 'job-running.json')), { code: 'ENOENT' });
    assert.deepEqual(await fs.readdir(folder), ['unrelated.txt']);

    await install.click();
    await page.getByText('Instalando o jogo…', { exact: true }).waitFor({ timeout: 5000 });
    await fs.writeFile(path.join(folder, 'allow-install'), '');
    await page.getByText('Instalação concluída', { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isEnabled(), true);
    assert.deepEqual(JSON.parse(await fs.readFile(settings, 'utf8')), {
      desktop_shortcut: false, channel: 'preview', auto_check: false, game_dir: path.join(folder, 'NX2Saria'), dump: valid,
    });

    await page.getByRole('button', { name: 'Recompilar o jogo' }).click({ timeout: 5000 });
    await page.getByText('Recompilação concluída', { exact: true }).waitFor({ timeout: 10000 });
    await fs.access(path.join(folder, 'NX2Saria', 'rebuilt'));
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isEnabled(), true);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: rebuild requested/);
    assert.match(log, /electron: rebuild ended outcome=complete/);
    assert.doesNotMatch(log, /electron: play requested/);

    const oldMarker = path.join(root, 'data', 'job-running.json');
    await fs.writeFile(oldMarker, '{broken marker');
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, root);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.getByText('Não foi possível verificar a compilação anterior', { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    assert.equal(await fs.readFile(oldMarker, 'utf8'), '{broken marker');
    assert.equal(await page.getByRole('button', { name: 'Parar compilação' }).isVisible(), false);
    await app.evaluate(({ dialog }) => {
      dialog.showMessageBox = async () => ({ response: 0, checkboxChecked: false });
    });
    const confirm = page.getByRole('button', { name: 'Recuperar instalação' });
    await confirm.click({ timeout: 5000 });
    await page.waitForFunction(() => !document.querySelector('#confirm-recovery').disabled);
    assert.equal(await fs.readFile(oldMarker, 'utf8'), '{broken marker');
    await app.evaluate(({ dialog }) => {
      dialog.showMessageBox = async () => ({ response: 1, checkboxChecked: false });
    });
    await confirm.click();
    await page.getByText(/A compilação anterior não terminou/).waitFor({ timeout: 5000 });
    await assert.rejects(fs.access(oldMarker), { code: 'ENOENT' });

    async function previousBuild() {
      const token = randomUUID().replaceAll('-', '');
      const run = path.join(root, 'data', 'build-supervisor', token);
      const release = path.join(run, 'release');
      const begun = path.join(run, 'ready');
      await fs.mkdir(run, { recursive: true });
      const pythonExe = process.env.NX2SARIA_PYTHON || 'python3';
      const builder = 'import pathlib,sys,time\na,b=map(pathlib.Path,sys.argv[1:])\na.touch()\n'
        + 'end=time.monotonic()+15\nwhile not b.exists() and time.monotonic()<end: time.sleep(.05)';
      const supervisor = spawn(pythonExe, ['-m', 'launcher.build_supervisor', '--install-root', root,
        '--run-dir', run, '--', pythonExe, '-c', builder, begun, release], {
        detached: true, cwd: repository, stdio: ['pipe', 'pipe', 'ignore']
      });
      previousBuilds.push({ release, ended: once(supervisor, 'exit') });
      const input = readline.createInterface({ input: supervisor.stdout });
      try {
        const [line] = await once(input, 'line', { signal: AbortSignal.timeout(5000) });
        const build = { ...JSON.parse(line), version: 1, token };
        await fs.writeFile(oldMarker, JSON.stringify({ build, kind: 'rebuild', task: 'test build' }));
        supervisor.stdin.end('start\n');
      } finally { input.close(); }
      const until = Date.now() + 5000;
      while (Date.now() < until) {
        try { await fs.access(begun); break; } catch {}
        await new Promise(resolve => setTimeout(resolve, 30));
      }
      await fs.access(begun);
      await page.getByRole('button', { name: 'Selecionar instalação' }).click();
      await page.getByText('Uma compilação anterior ainda está rodando', { exact: true }).waitFor({ timeout: 5000 });
      assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
      return release;
    }

    const release = await previousBuild();
    await page.getByRole('button', { name: 'Aguardar', exact: true }).click();
    await page.getByText('Aguardando a compilação anterior terminar…').waitFor({ timeout: 5000 });
    await fs.writeFile(release, '');
    await page.getByText(/A compilação anterior não terminou/).waitFor({ timeout: 5000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isEnabled(), true);

    await previousBuild();
    await app.evaluate(({ dialog }) => {
      dialog.showMessageBox = async () => ({ response: 1, checkboxChecked: false });
    });
    await page.getByRole('button', { name: 'Parar compilação', exact: true }).click();
    await page.getByText(/A compilação anterior não terminou/).waitFor({ timeout: 5000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isEnabled(), true);
    await assert.rejects(fs.access(oldMarker), { code: 'ENOENT' });

    await page.getByRole('button', { name: 'Buscar atualizações' }).click({ timeout: 5000 });
    await page.getByText('Versão 999.0.0 disponível', { exact: true }).waitFor({ timeout: 5000 });
    await page.getByText('A synthetic update note.', { exact: true }).waitFor({ timeout: 5000 });
    assert.deepEqual(feedRequests, ['/manifest.json']);
    assert.equal((await fs.readFile(path.join(root, 'app', 'current.txt'), 'utf8')).trim(), '0.0.1');

    const game = path.join(folder, 'NX2Saria');
    const slot = path.join(game, 'profile', 'save', '01'.repeat(16), 'slot_00');
    await fs.mkdir(slot, { recursive: true });
    await fs.writeFile(path.join(game, 'profile', 'account.uid'), Buffer.alloc(16, 1));
    await fs.writeFile(path.join(slot, 'progress.sav'), 'synthetic save');
    const exportFolder = path.join(base, 'saved-exports');
    await fs.mkdir(exportFolder);
    await page.getByRole('button', { name: 'Saves', exact: true }).click({ timeout: 5000 });
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, exportFolder);
    await page.getByRole('button', { name: 'Escolher pasta de exportação' }).click();
    await page.getByText(exportFolder, { exact: true }).waitFor({ timeout: 5000 });
    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await page.getByRole('button', { name: 'Escolher pasta de exportação' }).click();
    await page.waitForFunction(() => !document.querySelector('#choose-save-export').matches(':disabled'));
    await page.getByRole('checkbox', { name: 'Incluir log e configurações' }).uncheck();
    await page.getByRole('button', { name: 'Exportar save' }).click();
    await page.getByText(/Save exportado/).waitFor({ timeout: 5000 });
    const saved = await fs.readdir(exportFolder);
    assert.equal(saved.length, 1);
    assert.match(saved[0], /^NX2Saria-save-.*\.zip$/);
    assert.doesNotMatch(saved[0], /bug-report/);
    assert.equal(await fs.readFile(path.join(slot, 'progress.sav'), 'utf8'), 'synthetic save');
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).save_export_dir, exportFolder);

    const currentSave = path.join(slot, 'progress.sav');
    await fs.writeFile(currentSave, 'newer local save');
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, path.join(exportFolder, saved[0]));
    await page.getByRole('button', { name: 'Importar save', exact: true }).click({ timeout: 5000 });
    await page.getByText(/Save importado/).waitFor({ timeout: 5000 });
    assert.equal(await fs.readFile(currentSave, 'utf8'), 'synthetic save');
    const inbox = path.join(game, 'saves');
    const backup = (await fs.readdir(inbox)).find(name => name.startsWith('backup '));
    assert.ok(backup);
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, path.join(inbox, backup));
    await page.getByRole('button', { name: 'Importar pasta do save' }).click();
    await page.getByText(/Save importado/).waitFor({ timeout: 5000 });
    assert.equal(await fs.readFile(currentSave, 'utf8'), 'newer local save');
    await page.getByRole('button', { name: 'Abrir pasta dos saves' }).click();
    await page.getByText('Pasta dos saves aberta.', { exact: true }).waitFor({ timeout: 5000 });
    await page.getByRole('button', { name: 'Configurações', exact: true }).click({ timeout: 6000 });
    const panel = page.locator('#settings-panel');
    const installedChannel = panel.getByLabel('Canal de atualizações');
    assert.equal(await installedChannel.inputValue(), 'preview');
    await installedChannel.selectOption('stable');
    await panel.getByLabel('Instalar atualizações automaticamente').uncheck();
    await panel.getByLabel('Manter cache de compilação').uncheck();
    await page.waitForFunction(() => !document.querySelector('#settings-keep-cache').matches(':disabled'));
    const updated = JSON.parse(await fs.readFile(settings, 'utf8'));
    assert.equal(updated.channel, 'stable');
    assert.equal(updated.auto_install, false);
    assert.equal(updated.keep_cache, false);
    assert.equal(updated.game_dir, game);
    assert.equal(updated.save_export_dir, exportFolder);
    assert.deepEqual(feedRequests, ['/manifest.json']);
    assert.equal(await panel.getByText(game, { exact: true }).count(), 1);

    const previousDump = await panel.locator('#settings-dump-name').textContent();
    const chooseDump = panel.getByRole('button', { name: 'Escolher dump do jogo' });
    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await chooseDump.click();
    await page.waitForFunction(() => !document.querySelector('#settings-dump').disabled);
    assert.equal(await panel.locator('#settings-dump-name').textContent(), previousDump);
    const invalidDump = path.join(base, 'invalid.nup');
    await fs.writeFile(invalidDump, 'not a game dump');
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, invalidDump);
    await chooseDump.click();
    await page.locator('#settings-result').getByText(/não é um dump válido/i).waitFor({ timeout: 5000 });
    assert.equal(await panel.locator('#settings-dump-name').textContent(), previousDump);
    const nextDump = path.join(base, 'replacement.nup');
    await fs.copyFile(updated.dump, nextDump);
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, nextDump);
    await chooseDump.click();
    await panel.getByText('replacement.nup', { exact: true }).waitFor({ timeout: 5000 });
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).dump, nextDump);
    await panel.getByRole('button', { name: 'Abrir pasta dos logs' }).click();
    await page.locator('#settings-result').getByText('Pasta dos logs aberta.', { exact: true }).waitFor({ timeout: 5000 });
    const workerLog = await fs.readFile(path.join(root, 'data', 'logs', 'launcher.log'), 'utf8');
    const electronLog = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(workerLog, /worker: settings dump result=saved/);
    assert.match(electronLog, /electron: settings open target=logs result=ok/);
    assert.doesNotMatch(electronLog, /replacement\.nup|nx2-first-run-ui-/);
  } finally {
    if (app) await app.close();
    server.close();
    for (const previous of previousBuilds) {
      await fs.writeFile(previous.release, '');
      await previous.ended;
    }
    await fs.rm(base, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
