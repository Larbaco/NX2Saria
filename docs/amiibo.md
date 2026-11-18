<!-- languages -->
**English** · [Português (Brasil)](pt-BR/amiibo.md) · [Русский](ru/amiibo.md) · [简体中文](zh-Hans/amiibo.md)
<!-- /languages -->

# Amiibo

The port has no NFC reader of its own: it puts an amiibo dump file on an emulated reader when the game asks for an
amiibo. Every `nn::nfp` call the game makes is answered by the runtime (`runtime/ov_system.c`, `runtime/amiibo.c`).
You need your own dump of your own figure: a 540-byte NTAG215 image as `.bin` or `.nfc`.

## Picking an amiibo

When the game asks for an amiibo, the list of dumps opens over the game by itself. **A** (or a click) puts the
highlighted figure on the reader and closes the list; **B** closes it without one. It opens again the next time the
game asks, with the cursor on the figure it was on last time (until the game is closed). Turn it off under
**Advanced** in the Amiibo tab (`amiibo_popup = 0`).

## The daily limit

The game's amiibo table (`Game/AmiiboSetting/AmiiboSetting.game__ui__AmiiboSetting.bgyml`) gives the two figures with
a mask (numbers `0x04FD` and `0x04FF`) their mask at every tap. The figures of the Zelda series, and Link, Zelda and
Ganon of the other series, give three to five random items (hearts, magic, ammunition) once a day; after that the game
says "You cannot use this amiibo anymore today". Other figures give nothing.

**Amiibo daily limit** in the **Game** tab (`amiibo_daily_limit`) is on by default, as in the game. Off, every tap
gives items: the game reads each drop table's `EnableDailyLimit` when it reads a figure, and a code patch (the patch
set `amiibo_daily`, `recomp/patches-v111.txt`) makes it read false, as the mask figures' tables say. The change applies
to the next figure the game reads, without a restart. The table itself is not changed, so a mod's drops stay. Game
version 1.1.1 only; on other versions the row is not shown.

## The Amiibo tab

Open the settings menu (F1) and pick the **Amiibo** tab.

- The list on the left shows every 540-byte `.bin` and `.nfc` file in the amiibo folder and its subfolders (three
  levels down), by name; other files stay out of it. **A** or a click on a figure loads the dump and puts it on the
  reader. The search field above it narrows the list. The figure last loaded has a dot: cyan while it is on the reader,
  gold after it left. The list opens with the cursor where it was last, here or over the game.
- **Reader** shows what the game does: ready, waiting for an amiibo, amiibo on the reader, or reading it. Under it, the
  last action, and the figure last loaded: its nickname, whether it holds game data, and **Lift the amiibo**.
- **Advanced**:
  - **Amiibo folder**: `amiibo` in the folder the game starts in (the game's folder when the launcher starts it).
    **Open** picks another one, **Default folder** goes back, **Refresh** reads the folder again (the tab also reads
    it whenever it opens). The choice is saved as `amiibo_dir`.
  - **Show the amiibo list when the game asks for an amiibo** (`amiibo_popup`).
  - With **Lift the amiibo by itself after a tap** on, the figure leaves the reader after the time set under it
    (1 to 6 seconds), as if you had held it there and taken it away.
  - The figure's file, character, series, number and type, its UID and its own data (below), with **Erase game data**.
  - The keys file and write-back (below).

## What the game reads and writes

The game reads the figure's model (character, series, number, type) and its UID, the owner's nickname and the
figure's write count and dates. It can also keep a small block of game data on the figure (at most 216 bytes) under
its own access ID: it creates that block the first time, then reads and replaces it.

The port keeps each figure's data, game data included, in `profile/amiibo/<uid>.dat` in the game's folder, named
after the figure's UID (14 hex digits). It is read again whenever the same figure is tapped, from the same dump or
another dump of the same figure, and it wins over the data in the dump. Without that file the figure's data comes from
the dump when the keys (below) decrypt it; otherwise the figure starts as a new one: registered, its nickname taken
from the file name (the first 10 characters), no game data.

The tab shows that data for the figure on the reader:

- **Nickname**, or that no owner and nickname are set.
- **Figure data**: new (nothing saved yet), saved by the port (`<uid>.dat`), or read from the dump (with the keys,
  below).
- Whether the keys decrypted the dump.
- The number of writes and the last write date.
- **Game data**: none, or its size and the access ID it belongs to.
- **Erase game data** removes the game data from the figure and saves it (into the dump too when write-back is on).
  Press it twice: the first press arms it for three seconds. The game can create new game data the next time it uses
  the figure.

## Keys file

Without keys the port reads only the UID and the model of a dump: a figure a game wrote on a console starts as new.
With your own keys file the port decrypts the dump and checks its signatures, so the nickname, write count, dates and
the game data on the figure carry over, and the game's changes can go back into the dump (write-back, below).

The keys file is 160 bytes. The port looks for `key_retail.bin` in the amiibo folder; `amiibo_keys` in `nx2saria.ini`
names another file. The port reads it the first time the Amiibo tab opens or a dump is loaded. The tab shows whether
keys are loaded and which file is in effect; **Reload keys** reads it again (after you put the file in place or changed
the folder or `amiibo_keys`) and decrypts the figure on the reader once more. Key bytes never appear in the log or
anywhere else.

## Write-back

**Write the game's changes into the dump file** (`amiibo_writeback = 1`, off by default) also writes the game's
changes into the dump file, encrypted and signed again, so that the dump keeps them for other programs. It needs the
keys and a dump they decrypted; without them the changes go only to `profile/amiibo/<uid>.dat`. Before the first
write into a dump the port copies it once to `<dump>.bak` (an existing `.bak` is never replaced).

## Settings

In `nx2saria.ini` ([nx2saria.ini.example](../nx2saria.ini.example), `[amiibo]`) or as `NX2SARIA_*` environment variables
(`NX2SARIA_AMIIBO_DIR`, ...). All of them apply while the game runs; a changed keys file is read at **Reload keys**. The older `nx2navi.ini` and `NX2NAVI_*` names still work ([Names and compatibility](branding.md)).

| Key | Default | |
| --- | --- | --- |
| `amiibo_daily_limit` | `1` | `0`: no figure has the game's once-a-day limit (the Game tab's **Amiibo daily limit**; 1.1.1). |
| `amiibo_dir` | `amiibo` | The folder the Amiibo tab lists; the keys file's default place. |
| `amiibo_keys` | `key_retail.bin` in the amiibo folder | Your keys file. |
| `amiibo_popup` | `1` | `0`: the list does not open over the game when it asks for an amiibo. |
| `amiibo_writeback` | `0` | `1`: the game's changes also go into the dump file. |

## Limits

- No physical NFC reader: only dump files.
- The owner's Mii the game sees is a default one, not the Mii stored on the figure.
- The owner and nickname come from the figure's data or the file name. When the game opens the console's amiibo
  settings, they answer at once without a screen of their own: registering owner and nickname keeps the current ones,
  erasing game data erases it as the tab does, and restoring changes nothing.
