const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-platform-'));
  const repo = path.resolve(__dirname, '../..');
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const receipt = path.join(base, 'policykit-invocations');
  const python = path.join(base, 'worker-python');
  let app;
  try {
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ auto_check: false, game_dir: game, telemetry: 'off' }));
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'not an executable');
    await fs.writeFile(python,
      '#!/bin/sh\nshift 2\nexec "$NX2_TEST_PYTHON" "$NX2_TEST_UDMABUF_FIXTURE" "$NX2_TEST_UDMABUF_RECEIPT" "$@"\n',
      { mode: 0o755 });
    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, GSETTINGS_BACKEND: 'memory',
        NX2SARIA_TEST_LOCALE: 'pt-BR', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', NX2SARIA_TELEMETRY_URL: 'none',
        PYTHONPATH: [repo, process.env.PYTHONPATH].filter(Boolean).join(path.delimiter),
        NX2SARIA_PYTHON: python, NX2_TEST_PYTHON: process.env.NX2SARIA_PYTHON || 'python3',
        NX2_TEST_UDMABUF_RECEIPT: receipt, NX2_TEST_UDMABUF_FIXTURE: path.join(repo, 'tests', 'fixtures', 'worker_udmabuf.py') } });
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada', { exact: true }).waitFor();
    await page.getByRole('button', { name: 'Configurações', exact: true }).click();
    const offer = page.getByRole('button', { name: 'Aplicar correção do udmabuf', exact: true });
    const remove = page.getByRole('button', { name: 'Remover correção do udmabuf', exact: true });
    const result = page.locator('#settings-result');
    await app.evaluate(({ dialog }) => {
      dialog.showMessageBox = async (_, options) => {
        globalThis.udmabufPrompt = options;
        return { response: globalThis.udmabufContinue ? 0 : 1 };
      };
    });
    await offer.click();
    await result.getByText('Nenhuma mudança no sistema.').waitFor();
    await assert.rejects(fs.access(receipt), { code: 'ENOENT' });
    const prompt = await app.evaluate(() => globalThis.udmabufPrompt);
    assert.equal(prompt.title, 'Aplicar correção do udmabuf');
    assert.match(prompt.detail, /autorização de administrador/);
    assert.equal(prompt.defaultId, 1);
    assert.equal(prompt.cancelId, 1);
    await app.evaluate(() => { globalThis.udmabufContinue = true; });
    await offer.click();
    await result.getByText(/udmabuf está acessível/).waitFor();
    assert.equal(await fs.readFile(receipt, 'utf8'), 'apply\n');
    await remove.click();
    await result.getByText('Nenhuma mudança no sistema.').waitFor();
    assert.equal(await fs.readFile(receipt, 'utf8'), 'apply\nremove\n');
    await remove.click();
    await result.getByText('A correção do udmabuf foi removida.').waitFor();
    await offer.waitFor();
    assert.equal(await fs.readFile(receipt, 'utf8'), 'apply\nremove\nremove\n');
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /udmabuf action=apply result=cancelled/);
    assert.match(log, /udmabuf action=remove result=removed/);
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
