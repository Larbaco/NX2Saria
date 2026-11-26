const assert = require('node:assert/strict');
const test = require('node:test');
const { systemLocale, localeChoice, translateMessage } = require('../dist/locale');

test('matches supported system languages and keeps an English fallback', () => {
  assert.equal(systemLocale(['de-DE', 'ru-RU', 'pt-BR']), 'ru');
  assert.equal(systemLocale(['pt-PT']), 'pt-BR');
  assert.equal(systemLocale(['zh-CN']), 'zh-Hans');
  assert.equal(systemLocale(['zh-Hant-TW']), 'en-US');
  assert.equal(systemLocale([]), 'en-US');
  assert.equal(localeChoice('zh-Hans'), 'zh-Hans');
  assert.equal(localeChoice('../../settings'), 'automatic');
});

test('formats a translation without interpreting player data as another message', () => {
  const messages = { 'Save {name}': 'Salve {name}' };
  assert.equal(translateMessage(messages, 'Save {name}', { name: 'Save {name}' }), 'Salve Save {name}');
  assert.equal(translateMessage(messages, 'Unknown {amount}', { amount: 3 }), 'Unknown 3');
});

test('all translation tables use the same source messages and formatting fields', () => {
  const { messageTables } = require('../dist/messages');
  const sources = Object.keys(messageTables['en-US']).sort();
  const fields = text => [...new Set(Array.from(text.matchAll(/\{[a-zA-Z]\w*\}/g), match => match[0]))].sort();
  assert.ok(sources.length > 200);
  for (const locale of ['en-US', 'ru', 'zh-Hans']) {
    const messages = messageTables[locale];
    assert.deepEqual(Object.keys(messages).sort(), sources);
    for (const source of sources) {
      assert.ok(messages[source].length, source);
      assert.deepEqual(fields(messages[source]), fields(source), source);
    }
  }
});

test('the catalog covers static window labels and directly referenced messages', () => {
  const fs = require('node:fs');
  const path = require('node:path');
  const ts = require('typescript');
  const { messageTables } = require('../dist/messages');
  const sources = new Set(Object.keys(messageTables['en-US']));
  const directory = path.resolve(__dirname, '../src');
  const html = fs.readFileSync(path.join(directory, 'index.html'), 'utf8');
  const labels = [...html.matchAll(/<[^>]+\bdata-message(?:\s[^>]*)?>([^<]*)</g)];
  assert.ok(labels.length > 80);
  for (const [, label] of labels) assert.ok(sources.has(label), label);

  for (const name of ['appearance.ts', 'main.ts', 'renderer.ts', 'reports.ts']) {
    const source = ts.createSourceFile(name, fs.readFileSync(path.join(directory, name), 'utf8'), ts.ScriptTarget.Latest);
    function visit(node) {
      if (ts.isCallExpression(node) && ts.isIdentifier(node.expression) && node.expression.text === 'tr') {
        const argument = node.arguments[0];
        if (ts.isStringLiteralLike(argument)) assert.ok(sources.has(argument.text), name + ': ' + argument.text);
      }
      ts.forEachChild(node, visit);
    }
    visit(source);
  }
});
