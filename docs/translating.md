# Translating the docs

The player docs are translated into the languages of the settings menu: Brazilian Portuguese (`pt-BR`), Russian (`ru`)
and Simplified Chinese (`zh-Hans`). English is the source. Developer docs (building, architecture, the renderer notes,
the studies, the code notes of `docs/code/`), the release notes and the changelogs stay in English only: the launcher
shows the release notes as they are, in one language. `check` refuses a translation of a page of `docs/code/`.

## Layout

| English source | Translation |
|---|---|
| `README.md` | `README.<lang>.md`, beside it |
| `docs/<name>.md` | `docs/<lang>/<name>.md` |

Translated now: `README.md`, `docs/install.md`, `docs/known-issues.md`, `docs/controls.md`, `docs/amiibo.md` and
`docs/telemetry.md`. English paths never move for a translation: release notes, update manifests and code comments link
to them.

## A translation's first lines

```markdown
<!-- translation: lang=pt-BR source=docs/install.md source-hash=<hash> reviewed=no -->
<!-- languages -->
...written by tools/docs_i18n.py...
<!-- /languages -->
# Title in the translation's language
```

- **The header** names the language, the English source and `source-hash`, the SHA-1 of the English file the
  translation was made from, with its language block left out (adding a language changes only the block).
  `python tools/docs_i18n.py stamp` writes it. `reviewed=yes` once a speaker of the language has read it through.
- **The language block** links the page's other languages and, in a translation, says that the English version
  prevails when they differ, and that the translation is not reviewed yet while `reviewed=no`. Never edit it by hand:
  `python tools/docs_i18n.py bars --write` writes it on every page of the set, the English one included. In a README the
  block sits between the release badge and the links under it; elsewhere it comes first, after the header.

## What stays as it is

- Code, setting keys and values (`tracker_api = 1`), environment variables, file and folder names, command lines, log
  lines, URLs, issue and pull request references.
- Product and project names: NX2Saria, NX2navi, Ocarina Atlas, hyrule-tracker, Vulkan, DLSS, FSR, amiibo, RADV and
  the like. The game's title stays *The Legend of Zelda: Ocarina of Time*.
- Key and button names written on a keyboard or pad (F1, Ctrl+F5, Minus, Plus, A, B, ZL).
- Headings may be translated: anchors follow the translated heading. A link into another page of the same language
  uses that page's translated anchor; `tools/docs_i18n.py check` finds a wrong one.

## Interface labels

Quote every label exactly as the player sees it in the translation's language:

- **Settings menu and overlay:** `runtime/ui/ui_text.c`, the arrays `text_pt_br`, `text_ru` and `text_zh_hans`. Find the
  English label in `text_en`, then the same `UI_...` id in the language's array. When the language has no entry for
  that id, the menu shows the English text: quote it in English.
- **Launcher:** English only; quote its labels in English.

A label is set in italics or bold as in the English page.

## Links

- A link to a page that has a translation in the same language goes to that translation; `check` refuses a link to
  the English page then.
- A link to a page that exists in English only keeps the English page and says so after the link: "(em inglês)",
  "(на английском)", "（英文）".
- Relative paths change with the folder: from `docs/pt-BR/install.md`, `../launcher.md` is the English launcher guide
  and `../../README.pt-BR.md` the Portuguese README.

## Style

- Write for players, in the plain register of the English page. Keep its meaning and its facts: no claim, number or
  promise that the English page does not make, and nothing left out. The legal and content notices are translated
  faithfully, and the README's license section says that the English text is the one that applies.
- Keep the structure: the same sections in the same order, so a reader can compare.
- Typography of the language: Chinese uses full-width punctuation (，。：；（）) around Chinese text, with a space between
  Chinese and Latin words or numbers. Chinese paragraphs are not wrapped by hand: GitHub shows a line break inside a
  paragraph as a space, which does not belong between Chinese characters. Bold or italics that start or end next to
  Chinese text need their punctuation outside the markers, or a space before a link inside them, to render.

### Terms

| English | pt-BR | ru | zh-Hans |
|---|---|---|---|
| dump (of the game, of an amiibo) | dump | дамп | 转储 |
| setup (the Windows installer) | instalador | установщик | 安装程序 |
| launcher | launcher | лаунчер | 启动器 |
| build (the game, verb) | compilar | собрать | 构建 |
| upscaling | upscaling | апскейлинг | 超分辨率 |
| frame generation | geração de quadros | генерация кадров | 帧生成 |
| shader / pipeline | shader / pipeline | шейдер / конвейер (pipeline) | 着色器 / 管线 |
| log | log | журнал (лог) | 日志 |
| driver | driver | драйвер | 驱动程序 |
| save | save (jogo salvo) | сохранение | 存档 |
| mod | mod | мод | 模组 |
| HUD | HUD | интерфейс (HUD) | HUD |
| tracker | tracker | трекер | 追踪器 |
| pre-release | pré-lançamento | предварительный выпуск | 预发布版本 |
| issue tracker | rastreador de issues | трекер задач | 问题跟踪器 |

The menu's own words win over this table: where the menu says something else for the same thing, use the menu's.

## Keeping translations current

When an English page that has translations changes:

1. `python tools/docs_i18n.py status` lists each translation as `ok` or `stale`. The content guard workflow runs
   `check`, which fails on broken links, anchors and language blocks, and warns about stale translations without
   failing, so that an English change is never blocked by a language its author does not write.
2. Whoever updates a translation runs `python tools/docs_i18n.py diff docs/pt-BR/install.md` to see what changed in
   English since it was made, brings the translation up to date, then `python tools/docs_i18n.py stamp
   docs/pt-BR/install.md`. A reviewer who read it through adds `--reviewed`.
3. A translation that stays stale keeps its notice that the English version prevails.

Corrections and new translations are welcome as pull requests; [CONTRIBUTING.md](../CONTRIBUTING.md) has the rest.
