const fs = require('node:fs');
const { execFileSync } = require('node:child_process');
for (const file of ['index.html', 'style.css']) fs.copyFileSync(`src/${file}`, `dist/${file}`);
let build = 'unknown';
try {
  build = execFileSync('git', ['rev-parse', '--short=8', 'HEAD'], { cwd: __dirname, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
} catch {}
fs.writeFileSync('dist/build-id.txt', build + '\n');
