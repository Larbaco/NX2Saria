const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const http = require('node:http');
const { once } = require('node:events');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-report-ui-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const settings = path.join(root, 'data', 'launcher.json');
  const logs = path.join(game, 'logs');
  const repo = path.resolve(__dirname, '../..');
  const posts = [];
  const server = http.createServer((request, response) => {
    posts.push(request.url);
    response.writeHead(201).end();
  });
  let app;
  try {
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'));
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(logs);
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(settings, JSON.stringify({ game_dir: game }));
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    const fixture = execFileSync('python3', ['-c', `
import json,sys
from pathlib import Path
from launcher import telemetry
game=Path(sys.argv[1])
summary=telemetry.example_summary()
(game/'logs'/'session-summary.json').write_text(json.dumps(summary))
sys.stdout.buffer.write(telemetry.body(telemetry.payload(summary,Path('VERSION').read_text().strip())))
`, game], { cwd: repo, encoding: 'utf8' });
    const launch = endpoint => _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, GSETTINGS_BACKEND: 'memory',
        NX2SARIA_TELEMETRY_URL: endpoint, NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0' } });

    app = await launch(`http://127.0.0.1:${server.address().port}`);
    const page = await app.firstWindow();
    const reports = page.locator('#reports-dialog');
    await reports.getByText('Ajudar a melhorar o NX2Saria?', { exact: true }).waitFor({ timeout: 6000 });
    await reports.getByText(/em \d+ s/).waitFor();
    await reports.getByRole('button', { name: 'Ver o que é enviado' }).click();
    await reports.locator('#reports-preview-text').waitFor();
    assert.equal(await reports.locator('#reports-preview-text').textContent(), fixture);
    await reports.getByRole('button', { name: 'Voltar' }).click();
    await reports.getByText(/ao escolher Enviar ou fechar esta janela/).waitFor();
    await reports.getByRole('button', { name: 'Não enviar', exact: true }).click();
    await reports.waitFor({ state: 'hidden' });
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).telemetry, 'off');
    await fs.access(path.join(logs, 'session-summary.sent.json'));
    await assert.rejects(fs.access(path.join(logs, 'session-summary.json')), { code: 'ENOENT' });

    await page.getByRole('button', { name: 'Configurações', exact: true }).click();
    await page.getByRole('button', { name: 'Relatórios de sessão', exact: true }).click();
    const consent = reports.getByRole('checkbox', { name: 'Enviar relatórios de sessão' });
    assert.equal(await consent.isChecked(), false);
    await consent.check();
    await page.waitForFunction(() => !document.querySelector('#reports-enabled').disabled);
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).telemetry, 'on');
    await reports.getByRole('button', { name: 'Escrever comentário' }).click();
    await reports.getByText(/envio .*desativado.*pacote de teste/i).waitFor();
    assert.equal(await reports.getByRole('button', { name: 'Enviar comentário' }).isDisabled(), true);
    assert.deepEqual(posts, []);
    await app.close();

    app = await launch('none');
    const unavailable = await app.firstWindow();
    await unavailable.getByText('Instalação encontrada', { exact: true }).waitFor();
    await unavailable.getByRole('button', { name: 'Configurações', exact: true }).click();
    assert.equal(await unavailable.getByRole('button', { name: 'Relatórios de sessão' }).count(), 0);
  } finally {
    if (app) await app.close().catch(() => {});
    server.close();
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
