# Personal folders in logs and reports

Players attach `logs/nx2saria.log`, the F8 capture's files and the launcher's logs to public issues. Those used to name
the account, and a computer's name often holds its owner's ("maria-pc"): `C:\Users\<name>\AppData\Local\Programs\NX2Saria\game\SDL3.dll`, `/home/<name>/Games/...`. They now
write the account's folders short, and keep the rest of each path, which still says where a file was:

| Folder | Written as |
|---|---|
| `C:\Users\<name>\AppData\Local` | `%LOCALAPPDATA%` |
| `C:\Users\<name>\AppData\Roaming` | `%APPDATA%` |
| `C:\Users\<name>` (also `HOMEDRIVE` + `HOMEPATH` and the 8.3 form `C:\Users\NAME~1`) | `%USERPROFILE%` |
| a profile on a network share (`HOMESHARE`) | `%HOMESHARE%` |
| `OneDrive`, `TEMP` / `TMP` | `%OneDrive%`, `%TEMP%` |
| the Linux home (`HOME`, and the folder it resolves to, such as `/var/home/<name>`); under Wine also `Z:\home\<name>` | `~` |
| the account's name alone inside a path | `%USERNAME%` (Windows), `$USER` (Linux) |
| the computer's name (`COMPUTERNAME`, `USERDOMAIN` on a PC outside a domain, the Linux host name and its first label, `HOSTNAME`) anywhere in a line: `\\<host>\share`, a network line | `%COMPUTERNAME%` |
| a Windows domain's name (`USERDOMAIN` when it is not the computer's) | `%USERDOMAIN%` |

## Where

- The game: every log line goes through `main.c` `rt_log_raw`, which runs it through `runtime/log_privacy.c`. So the
  log file, stderr, the lines written before the log file opens, the queue the Windows exit note reads and the crash
  report all get the short form. The bug capture (`bugcap.c`) also writes `log-tail.txt`'s header line and the
  setting values in `settings.ini` and `capture.txt` short; the game's bug report zip (`save_share.c`) its
  `nx2saria.ini`.
- The launcher and the build tools: `tools/private_paths.py`, with the same rules, in `launcher/context.py`
  (`launcher.log`), `launcher/jobs.py` `Job.log` (the job screen and Copy Report), `launcher/ui/app.py` `copy_report`,
  `launcher/start.pyw` (`crash.log` and its message box), `launcher/saves.py` (the bug report zip's log and settings,
  also when the log came from an older version or a run with the switch off), `tools/appimage_gui.py` (the AppImage's
  `build.log`) and `tools/make_game.py` (`button-prompts.txt`).

## What is not changed

- Paths handed to programs: the files the game opens, and the tracker API's `status.save_dir`
  ([tracker-api.md](tracker-api.md)). The tracker needs the real folder to read the save files; the API answers only
  on `127.0.0.1` and is off by default. The log's line about that answer is written short like any other.
- The menu's notes on screen ("Saved as ..."): the player needs the full path to find the file.
- The Windows installer's own logs.

## Matching

`runtime/log_privacy_core.h` (C) and `tools/private_paths.py` (Python) match the same way:

- A folder matches only whole: the character before it and the one after it are not part of a name (a letter, digit,
  `_`, `.`, `-`, or a UTF-8 byte). `/home/ana` leaves `/home/anabel` and `/var/home/ana` alone; `\\?\C:\Users\ana\x`
  matches. The longest folder that matches wins, so `%LOCALAPPDATA%` comes before `%USERPROFILE%`.
- The account's name alone, three characters or longer, is replaced only as a word of its own (no letter, digit, `_`
  or UTF-8 byte next to it) inside a path (the run of non-blank characters around it holds `/` or `\`). "Steam Deck"
  stays as it is for the account `deck`, and `banana` for the account `ana`.
- A `.` or `-` right after a folder ends it when no name character follows it (the end of a sentence: "saved in
  `C:\Users\ana.`"); `C:\Users\ana.CORP` is another folder, where the name rule below still writes the name short.
- The computer's name, three characters or longer, is replaced as a word of its own anywhere, without case on every
  system. Names a system gives every machine of its kind (`localhost`, `steamdeck`, `ubuntu`, ...;
  `priv_common_host`, `COMMON_HOSTS`) are not looked for.
- Windows paths match without case (ASCII letters), with either separator, and with a run of separators for one, as an
  escaped path in an error's text writes it (`'C:\\Users\\ana\\x'`).
- Names too common to be the account's alone (`user`, `admin`, `root`, `guest`, `deck`, `steamuser`, ...;
  `log_privacy_core.h` `priv_common`, `private_paths.py` `COMMON`) are not looked for alone, so `/run/user/1000` and
  `...\User Data` stay as they are; a path under their home still matches as a folder.
- A root (`/`, `C:\`) is never a folder to shorten.

The game reads the folders once, before its first log line (`log_privacy_init`: the environment, no allocation), and
adds their resolved forms after the settings are read (`log_privacy_start`). A line holding none of them costs one table
read per byte and is passed on as it is; the code allocates nothing and takes no lock, so the crash report uses it too.

## The switch

`log_short_paths = 1` in `nx2saria.ini` ([diagnostics]) is the default. `0` (or `NX2SARIA_LOG_SHORT_PATHS=0`) writes
paths in full, for debugging on one's own machine; the lines logged before the settings are read are short either way.
The launcher and the build tools follow `NX2SARIA_LOG_SHORT_PATHS=0` in their environment.

Log lines:

```
[host] log: personal folders are written short (log_short_paths = 1): %LOCALAPPDATA%, %APPDATA%, %USERPROFILE% and the like, 7 folders, the account name as %USERNAME% and the computer's name as %COMPUTERNAME%; the rest of each path is kept
[host] log: 41 lines and values had a personal folder written short this run
--- NX2Saria launcher 0.18.5 (release) in %LOCALAPPDATA%\Programs\NX2Saria; personal folders are written short (%USERPROFILE%, %LOCALAPPDATA%, the account name and the computer name; NX2SARIA_LOG_SHORT_PATHS=0 writes them in full)
```

With the switch off: `log: paths are written in full, the account and computer names included (log_short_paths = 0)`.

A capture's `settings.ini` with a setting that names a folder in the home (a `mods_dir` of one's own) holds the short
form, so a replay of that capture (`tools/capture_run.py`) does not find that folder. On another PC it would not have
found it anyway.

Tests: `tests/log_privacy_check.py` (the C side: Windows and Linux tables on any system, and on Linux the environment
through `runtime/log_privacy.c`) and `tests/private_paths_test.py` (the Python side, the launcher's log and its bug
report zip).
