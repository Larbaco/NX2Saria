# Tracker API

A map tracker running on the same PC (a program that follows Link on a map and ticks off collectibles as the game
records them) can read the game's memory while it runs. The tracker API answers it on `127.0.0.1`, read-only, with
the newline-delimited JSON protocol that trackers already speak to a console emulator's debug server. A tracker that
reads this game there reads it here too, with nothing to install and no memory-reading permission to grant.
[ocarina-atlas.md](ocarina-atlas.md) covers one such tracker.

A tracker that reads the save files instead (the game's progress as values known by the hashes of their names) asks
the same server for the same values in the running game, `nx2saria.game_data`, and needs neither memory addresses nor
the game's layout. [hyrule-tracker.md](hyrule-tracker.md) covers one such tracker.

The API is off by default. The code is `runtime/tracker_api.c`; the listener is `plat_tcp_listen_loopback` in
`runtime/platform/`.

## Turning it on

```ini
[tracker]
tracker_api = 1
# tracker_api_port = 6545
```

or `NX2SARIA_TRACKER_API=1` (and `NX2SARIA_TRACKER_API_PORT`) in the environment. Both settings are read at start.

With the API on, the game's stack and heap block is placed from 64 GiB (`0x1000000000`) instead of from 4 GiB
(`runtime/memory.c` `mem_init`). Trackers follow pointers from the game's `.bss` into its heap and take any pointer
below `0x1000000000` for none, as no console emulator puts a heap there. The game takes its heap wherever the kernel
gives it one, as on the console, so nothing else changes. When no 14 GiB block is free from 64 GiB, the block goes
where it goes with the API off, and the log says so (below).

## Safety

- **This PC only.** The socket is bound to `127.0.0.1` and to no other address. Nothing on the network can reach it.
- **Read-only.** No method writes. Unknown methods, write methods included, are refused. `nx2saria.game_data` reads
  the game data through the same guest-only copy as `read_memory`.
- **Guest memory only.** A read or a search covers only the game's own regions (its modules, stack, heap, mapped
  physical memory and thread storage) and, within them, what the game can read. The runtime's own memory, and
  anything it read (the keys, the settings), lies outside those regions and cannot be named, even where it sits
  between them below 2^39.
- **Web pages.** A page in a browser can make the browser send an HTTP request or a TLS handshake to the port, but
  can never read an answer: this is no HTTP server, and a connection whose first byte is not `{` closes at once.
- **Limits.** 4 clients at a time, request lines under 4096 bytes and finished within 5 s, reads of 256 KiB, search
  patterns of 64 bytes, 64 hits, a search budget of 3 s, one search at a time, 4 GiB/s read by a search and 8 GiB a
  minute by all of them, 60 s without a request before a client is closed, 2 s for a client to take an answer.
- **Other users of the PC.** Any program of the PC can connect while the API is on, including those of another user
  account. Leave it off on a shared machine.

## Cost

Nothing runs on the game's threads. One host thread listens and each client has its own; none of them holds a
virtual core. A read is a copy of at most 256 KiB that cannot fault (`plat_mem_read`). A tracker that polls every
120 ms with three small reads costs microseconds a second.

A search copies memory in pieces of 1 MiB and holds no lock of `runtime/memory.c`, so the game's mapping calls never
wait for it. Outside the modules it copies only the pages that hold memory now: a page the game never wrote reads zero,
and reading it would allocate it (on Linux the heap is a shared memory file; on Windows the pages are committed but not
yet backed). A search for main's header ends with the modules, a few tens of milliseconds. A tracker that cannot follow its pointers
(on the title screen, while loading) searches the heap every few seconds for its whole budget; the 8 GiB a minute
bounds that to about two seconds of reading a minute.

## Protocol

TCP, one JSON object per line (`\n`, a `\r` before it is accepted), UTF-8.

- **Request:** `{"id": <number or string>, "method": "<name>", "params": {...}}`. The `id` and `params` may be
  left out. A number id comes back as a JSON number (`1.5e1` as `15`), a string id as it was sent.
- **Answer:** one line per request, in the order the requests came. On success
  `{"id": <the request's id>, "result": {...}}`; otherwise
  `{"id": ..., "error": {"code": <n>, "message": "<text>"}}`. A request without an id gets an answer without one.
  Nothing is sent without a request.
- **Closing:** the connection closes without an answer when a line's first byte past blanks is not `{` (at once, before
  the line ends), when a line is not one JSON object (numbers as JSON spells them, nothing after the object), when a
  string id is longer than 61 characters, when 4096 bytes come without a line end, or when a request is left
  unfinished for 5 s. Blank lines are skipped.
- **Addresses** are guest addresses, which are host addresses in this runtime. In a request they are a hex string
  with `0x` (`"0x7003c4705c"`), a decimal string, or a JSON number. In an answer they are lowercase hex strings with
  `0x`.

| Error code | Meaning |
|---|---|
| -32602 | The parameters are missing, out of range or do not parse |
| -32001 | The memory asked for is not memory the game can read now |
| -32601 | Unknown method (every method that would write is unknown) |
| -32002 | The game has no memory for the answer now, or nothing to answer yet: asking again later may answer |
| -32003 | Not on this game build: asking again will not answer (`nx2saria.game_data` outside version 1.1.1, or a kind not where or not laid out as 1.1.1 keeps it, its section read the same twice) |

### status

The game and the API. Trackers call it about once a second.

```json
{"id":1,"method":"status","params":{"fps_window_ms":0}}
{"id":1,"result":{"powered_on":true,"program_id":"0400486024DEC000","emulator":"nx2saria","build_id":"e0f3ccfaf50065ae…","game_version":"1.1.1","port_version":"0.14.0","api_version":1,"read_only":true,"methods":["status","read_memory","search_memory","nx2saria.state","nx2saria.game_data"]}}
```

| Field | Meaning |
|---|---|
| `powered_on` | The game has presented a frame. Before that its memory holds nothing to track. |
| `program_id` | The game's title id. |
| `emulator` | Always `nx2saria`: what a tracker shows as the game's source. |
| `build_id` | main's build id (64 hex digits). Version 1.1.1's starts with `e0f3ccfa`. |
| `game_version` | `1.1.1` (or the other version the build was made from). |
| `port_version`, `api_version` | This port's version, and this API's (below). |
| `read_only` | Always `true`. |
| `methods` | The methods this server answers. `nx2saria.game_data` is listed where the game data is known (version 1.1.1). |

`fps_window_ms` and any other parameter are ignored.

### read_memory

```json
{"id":2,"method":"read_memory","params":{"address":"0x7000000000","size":8,"as":"hex"}}
{"id":2,"result":{"hex":"010000005c70c403"}}
```

`size` is 1 to 262144 bytes. `as` is `hex` (the default; nothing else is answered). Every byte of the range has to be
readable by the game, else the answer is error -32001. Heap pages the game never touched read as zero, as they would
for the game. The bytes are read while the game runs: a value the game is changing at that moment may be read half
old, half new, as from an emulator's debug server.

### search_memory

```json
{"id":3,"method":"search_memory","params":{"pattern":"4d4f44305467030124fb0f016c342501","regions":"all","max_hits":4,"budget_ms":1500}}
{"id":3,"result":{"hits":["0x7003c4705c"],"complete":true}}
```

| Parameter | Meaning |
|---|---|
| `pattern` | 1 to 64 bytes in hex (`0x` allowed). |
| `start`, `end` | Optional: only hits in `[start, end)`. |
| `regions` | `all` (the default): the game's modules first (main, then the others); when they hold the pattern the search ends there, else it goes on to the heap, then the stack, mapped memory and thread storage. `heap`: the heap alone. |
| `max_hits` | 1 to 64 (16 by default). |
| `budget_ms` | 1 to 3000 (1000 by default), counted from the request's arrival (waiting for another client's search included): the search stops when it runs out. |

`complete` is `false` when the budget ran out, or the searches' 8 GiB of the last minute, before every region was read.
Pages the game cannot read, pages it never committed, pages it never wrote (outside the modules), and pages the GPU
wrote and the CPU has not fetched yet are not searched. A pattern is found where it lies in one run of such pages.

### nx2saria.state

This port's own summary. It needs no knowledge of the game's structures, and its fields keep their meaning across
game updates, as far as this port supports them.

```json
{"id":4,"method":"nx2saria.state"}
{"id":4,"result":{"api_version":1,"powered_on":true,"presents":81234,"area":"HyruleField_Adult","player":{"x":12.5,"y":-3.25,"z":100.0,"rotation":[0,0,1,0,1,0,-1,0,0]},"game_data":true,"main":"0x7000000000","heap":{"base":"0x1030000000","size":"0x300000000"}}}
```

| Field | Meaning |
|---|---|
| `presents` | The game's frames so far, one per update of the game. |
| `area` | The area the game is in, by its name in the game's files (`HyruleField_Adult`, `DekuTree`…), or `null` before a save is loaded. Outdoor areas carry the age in their name. Version 1.1.1 only. |
| `player` | Link's position in metres, or `null` while there is no player. Version 1.1.1 only. |
| `player.rotation` | Link's rotation, a 3x3 matrix by rows as the game holds it (`debug_tools.c dbg_player_rot`), or `null`. Which axis is his forward one is to be confirmed on the bench (`tools/tracker_api_client.py --watch`). |
| `game_data` | The game has loaded its game data (it does when it loads a save), so `nx2saria.game_data` answers. Version 1.1.1 only (always `false` on the others). |
| `main` | main's base address. |
| `heap` | The heap's reserved range: its base, and the size the game may grow it to. |

### nx2saria.game_data

The game's progress values of one kind, each with the key the save files know it by: the MurmurHash3 (x86, 32 bits,
seed 0) of the value's name. The kinds are named and numbered as in the save files. The game keeps these values in its
game-data manager (`**[main+0x4c7e180]` in version 1.1.1); this method reads them there, so a tracker needs neither
the manager's place nor its layout, and gets what the next save will hold.

```json
{"id":5,"method":"nx2saria.game_data","params":{"kind":"Int","hash":"0x04735978"}}
{"id":5,"result":{"kind":"Int","format":1,"count":1,"record_size":8,"presents":81234,"digest":"ff669bf90fe06404","records":"7859730439000000"}}
{"id":6,"method":"nx2saria.game_data","params":{"kind":"Bool","if_digest":"5e2f…"}}
{"id":6,"result":{"kind":"Bool","format":1,"count":11292,"record_size":8,"presents":81240,"digest":"5e2f…","unchanged":true}}
```

| Parameter | Meaning |
|---|---|
| `kind` | `Bool`, `Int`, `Enum`, `String64`, `String64Array`, `Int64` or `Bool64bitKey`. Required. |
| `hash` | Optional: only the value with this key (1 to 32 bits; a number, or a string in hex with `0x` or decimal). A key the game does not have gives `count` 0. Not for `Bool64bitKey`. |
| `filter` | `Bool64bitKey` only: `set` (the default) for the objects whose flag is set, `all` for every placed object. |
| `if_digest` | Optional: the `digest` of an earlier answer. When the records are the same, the answer has `"unchanged":true` and no `records`. |

`records` is the bytes of every value in hex, sorted by key, each record little-endian:

| Kind | Record (`record_size`) |
|---|---|
| `Bool` | u32 key, u32 value, 0 or 1 (8) |
| `Int` | u32 key, s32 value (8) |
| `Enum` | u32 key, u32 the hash of the value's name, as in the save files (8) |
| `String64` | u32 key, 64 bytes of text, zero from its end on (68) |
| `String64Array` | u32 key, u32 n (at most 64), then n × 64 bytes of text (`record_size` 0: records vary) |
| `Int64` | u32 key, s64 value (12) |
| `Bool64bitKey` | u64 key (the placed object's 64-bit hash); u32 its class as the game holds it (bit 0x40: the game clears it again, a respawning object); u8 set, 0 or 1; 3 bytes 0 (16). Sorted by the u64 key. |

`format` is the records' layout above (1); a change of it gives another number, so a tracker checks it. `count` is the
number of records, `presents` the game's frames when they were read (the kinds are read one request at a time, at
different frames), and `digest` a 64-bit FNV-1a of the record bytes (16 lowercase hex digits). A tracker that polls
sends each kind's last digest and gets a short answer while nothing changed. The Ints change all the time (play
time), the others when the player does something.

The values are read as the game holds them now: a change the game has queued (it applies changes at its next update)
shows one update later. The game fills its game data when it loads a save; until then every kind answers -32002, and
once it has, a kind it holds nothing of answers `count` 0. -32002 also answers a kind the game changed while it was
read (its section's count, entries, table or size, a pair for an entry that is not there, an entry with two pairs, a
key twice, placed objects out of order), an entry whose vtable is still 0 (being made), a String64Array value of more
than 64 texts, or a value it cannot read now. -32003 answers a game version other than 1.1.1, or a kind that does not
read as 1.1.1 keeps it (a count or size out of range, an entry with another vtable than the kind's), only after its
section read the same twice, so a save being loaded is not taken for it. The message says which. A value is never
made up: a kind is answered whole or refused.

The other kinds of the save files (`Float`, `UInt`, the arrays of numbers, vectors, `Binary`…) are not answered: no
tracker asks for them yet. Their layouts were read in the game's code but not checked live; they can be added within
this version.

### Versions and frequency

`api_version` is 1. Fields and methods may be added within a version; a field that changes its meaning or goes away,
or a method that changes its answer, gives a new version. A tracker checks `api_version` in `status`.

The client sets the pace. One request is answered at a time per connection, and up to four connections are served at
the same time. A tracker that reads three values every 120 ms and the status once a second is well within what the API
answers; a read takes a fraction of a millisecond. An item tracker that asks every kind of `nx2saria.game_data` twice a
second, with their digests, gets the Ints (about 20 KiB of hex) and a few short answers; the first answer of `Bool`
is about 180 KiB and the placed objects with `filter` `all` (once) about 370 KiB. A request copies one kind (for Bool
about 265 KiB of entries and 175 KiB of table, a fraction of a millisecond), one request at a time.

## The log

All lines start with `tracker api:` (`logs/nx2saria.log`):

| When | Line |
|---|---|
| Start, API off | `tracker api: off (tracker_api = 1 in nx2saria.ini or NX2SARIA_TRACKER_API=1 lets a map tracker …)` |
| Start, API on | `tracker api: on, read-only, listening on 127.0.0.1:6545 (this PC only; status, read_memory, search_memory, nx2saria.state, nx2saria.game_data; API version 1); the game's heap at 0x1030000000, above the 0x1000000000 trackers take heap pointers from` (`below` when the block did not fit at 64 GiB) |
| Port taken | `tracker api: on, but 127.0.0.1:6545 is taken (error 98), by another program or a closing one; trying again every 5 s; the game's heap at …`, then `tracker api: listening on 127.0.0.1:6545 after 3 tries` |
| A client's first request | `tracker api: client 1 (127.0.0.1:50412) asks: its first request is status (1 open)` |
| A client leaves | `tracker api: client 1 (127.0.0.1:50412) left after 812.4 s, the client closed the connection: 21056 requests (status 812, read_memory 20230, search_memory 14), 3 refused, 1.21 MiB sent (0 open)` |
| First answers | `tracker api: first status answered: …`, `first read_memory answered: …`, `first search_memory answered: …`, `first search_memory that found its pattern: … the first at 0x7003c4705c …`, `first nx2saria.state answered: area …, Link at (…)` |
| First refusal of each kind | `tracker api: read_memory refused: 8 bytes at 0x1000 are not memory the game can read now (the first such answer; the closing lines count the others)` |
| Game data asked before a save is loaded | `tracker api: nx2saria.game_data asked before the game loaded its game data (the first such answer, for Bool; the closing lines count the others)` (once) |
| First game data of each kind | `tracker api: first nx2saria.game_data of Bool answered: 11292 values, 90336 bytes, in 0.42 ms` (one line per kind; ` (one hash)` after it when the first request asked one hash) |
| Game data that could not be answered | `tracker api: nx2saria.game_data Bool could not be answered: this kind's entries are not laid out as version 1.1.1 lays them out (error -32003; the first such answer for this kind and error)` (once per kind and error) |
| Port checks | `tracker api: a program connected and left without a request (a tracker checking whether the port is taken); later checks are counted in the summaries` |
| Not a request | `tracker api: client 5 sent something that is not a JSON request ("POST / HTTP/1.1."); closed. …` (once) |
| Searches' minute spent | `tracker api: searches read 8192 MiB in the last minute, the most they may; …` (once) |
| Accept errors | `tracker api: a connection could not be taken (error 24); …` (once); after 50 in a row `the listener failed 50 times in a row; making it again` |
| Every 10 min while clients ask | `tracker api: the last 10 min: 5012 requests (…), 0 refused, 0.29 MiB sent, 300 port checks, 1 client open` |

Client lines stop after 20 a session and summaries after 36; the line that reaches the limit says so.

## Checking it

- `python tests/tracker_api_check.py` builds `runtime/tracker_api.c` with the platform layer and a guest memory of its
  own at the game's addresses (no game files, no game build) and talks to it as a tracker does.
- `python tools/tracker_api_client.py [--port 6545] [--watch 30] [--save <game>/profile/save]`, with the game
  running and the API on, asks the game what a tracker asks and says whether each step works: the status, the heap's
  place, main by its header, Link by the tracker's chain and by the port's own, the game data and the Gold Skulltula
  count, every kind of `nx2saria.game_data` (with `--save`, compared with the newest save), and with `--watch` Link's
  heading from his rotation beside the way he moves.
