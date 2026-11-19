// tests/log_privacy_check.py builds this: runtime/log_privacy_core.h's tables and rewrite, with Windows and Linux tables
// on any system. Prints "case: ok" or "case: FAIL ..." per case and "log_privacy_check: ok|FAILED" at the end.
#include <stdio.h>
#include <string.h>

#include "log_privacy_core.h"

static int bad;

static void expect(const PrivTable *t, const char *what, const char *in, const char *want) {
    char out[4096];
    size_t n = strlen(in), at = 0, o = 0;
    unsigned hits = 0;
    while (at < n) o += priv_rewrite(t, in, n, &at, out + o, sizeof out - 1 - o, &hits);
    out[o] = 0;
    int found = priv_find(t, in, n) < n;
    int ok = !strcmp(out, want) && found == (hits > 0) && found == (strcmp(in, want) != 0);
    printf("%s: %s", what, ok ? "ok\n" : "FAIL\n");
    if (!ok) printf("  in:   %s\n  got:  %s\n  want: %s\n  find %d hits %u\n", in, out, want, found, hits), bad++;
}

// The rewrite in small pieces (a caller's buffer that fills): the same text as in one piece.
static void pieces(const PrivTable *t, const char *what, const char *in) {
    char one[4096], many[4096];
    size_t n = strlen(in), at = 0, o = priv_rewrite(t, in, n, &at, one, sizeof one - 1, NULL);
    one[o] = 0;
    size_t m = 0;
    at = 0;
    while (at < n) {
        size_t k = priv_rewrite(t, in, n, &at, many + m, PRIV_MARK_MAX, NULL);
        m += k;
    }
    many[m] = 0;
    int ok = !strcmp(one, many);
    printf("%s: %s", what, ok ? "ok\n" : "FAIL\n");
    if (!ok) printf("  one:  %s\n  many: %s\n", one, many), bad++;
}

int main(void) {
    static PrivTable w, l, s;

    priv_clear(&w, 1);
    priv_add(&w, "C:\\Users\\Ana Souza", PRIV_PREFIX, "%USERPROFILE%");
    priv_add(&w, "C:\\Users\\Ana Souza\\AppData\\Local", PRIV_PREFIX, "%LOCALAPPDATA%");
    priv_add(&w, "C:\\Users\\Ana Souza\\AppData\\Roaming\\", PRIV_PREFIX, "%APPDATA%");
    priv_add(&w, "C:\\Users\\ANASOU~1", PRIV_PREFIX, "%USERPROFILE%");
    priv_add(&w, "\\\\fs01\\perfis\\asouza", PRIV_PREFIX, "%HOMESHARE%");
    priv_add(&w, "Ana Souza", PRIV_NAME, "%USERNAME%");
    expect(&w, "win: LOCALAPPDATA before USERPROFILE",
           "[host] libs: SDL3 C:\\Users\\Ana Souza\\AppData\\Local\\Programs\\NX2Saria\\game\\SDL3.dll",
           "[host] libs: SDL3 %LOCALAPPDATA%\\Programs\\NX2Saria\\game\\SDL3.dll");
    expect(&w, "win: case and separators", "log: c:/users/ana souza/appdata/local/NX2Saria/game/logs/nx2saria.log",
           "log: %LOCALAPPDATA%/NX2Saria/game/logs/nx2saria.log");
    expect(&w, "win: APPDATA", "C:\\Users\\Ana Souza\\AppData\\Roaming\\x.ini", "%APPDATA%\\x.ini");
    expect(&w, "win: profile alone", "saved in C:\\Users\\Ana Souza\\Downloads\\NX2Saria-save.zip",
           "saved in %USERPROFILE%\\Downloads\\NX2Saria-save.zip");
    expect(&w, "win: profile at the end", "home C:\\Users\\Ana Souza", "home %USERPROFILE%");
    expect(&w, "win: 8.3 form", "temp C:\\Users\\ANASOU~1\\AppData\\Local\\Temp\\x", "temp %USERPROFILE%\\AppData\\Local\\Temp\\x");
    expect(&w, "win: long-path prefix", "\\\\?\\C:\\Users\\Ana Souza\\Games", "\\\\?\\%USERPROFILE%\\Games");
    expect(&w, "win: network profile", "\\\\fs01\\perfis\\asouza\\Documents\\a", "%HOMESHARE%\\Documents\\a");
    expect(&w, "win: another account that starts the same", "C:\\Users\\Ana Souza2\\x", "C:\\Users\\Ana Souza2\\x");
    expect(&w, "win: name in another drive's path", "D:\\Jogos\\Ana Souza\\oot\\x.log", "D:\\Jogos\\%USERNAME%\\oot\\x.log");
    expect(&w, "win: name in text without a path", "player Ana Souza pressed F8", "player Ana Souza pressed F8");
    expect(&w, "win: nothing to do", "[host] vk: 1440 x 900, 60 Hz, FIFO", "[host] vk: 1440 x 900, 60 Hz, FIFO");
    expect(&w, "win: two in one line", "C:\\Users\\Ana Souza\\a -> c:\\users\\ana souza\\appdata\\local\\b",
           "%USERPROFILE%\\a -> %LOCALAPPDATA%\\b");
    expect(&w, "win: escaped separators", "No such file: 'C:\\\\Users\\\\Ana Souza\\\\AppData\\\\Local\\\\x'",
           "No such file: '%LOCALAPPDATA%\\\\x'");
    expect(&w, "win: a period after the folder", "saved in C:\\Users\\Ana Souza.", "saved in %USERPROFILE%.");
    expect(&w, "win: a domain profile: the name alone", "C:\\Users\\Ana Souza.CORP\\x", "C:\\Users\\%USERNAME%.CORP\\x");
    pieces(&w, "win: in pieces", "a C:\\Users\\Ana Souza\\x b C:\\Users\\Ana Souza\\AppData\\Local\\y D:\\Ana Souza\\z");

    priv_clear(&l, 0);
    priv_add(&l, "/home/joão silva/", PRIV_PREFIX, "~");
    priv_add(&l, "/var/home/joão silva", PRIV_PREFIX, "~");
    priv_add(&l, "joão silva", PRIV_NAME, "$USER");
    expect(&l, "linux: home with accent and space", "[host] cfg: read /home/joão silva/Games/oot/nx2saria.ini: 12 keys",
           "[host] cfg: read ~/Games/oot/nx2saria.ini: 12 keys");
    expect(&l, "linux: resolved home", "/var/home/joão silva/x", "~/x");
    expect(&l, "linux: case counts", "/home/JOÃO SILVA/x", "/home/JOÃO SILVA/x");
    expect(&l, "linux: home of another account", "/home/joão silvas/x", "/home/joão silvas/x");
    expect(&l, "linux: home under another folder", "/mnt/home/joão silva/x", "/mnt/home/$USER/x");

    priv_clear(&s, 0);
    priv_add(&s, "/home/ana", PRIV_PREFIX, "~");
    priv_add(&s, "ana", PRIV_NAME, "$USER");
    expect(&s, "short name: inside another word", "/srv/banana/anabel/ana_x/x.ana/y", "/srv/banana/anabel/ana_x/x.$USER/y");
    expect(&s, "short name: in a word with an accent", "/srv/anaé/x", "/srv/anaé/x");
    expect(&s, "short name: a part of a path", "/tmp/claude-1000/-home-ana--t3/x", "/tmp/claude-1000/-home-$USER--t3/x");
    expect(&s, "short name: at the end", "/srv/ana", "/srv/$USER");
    expect(&s, "short name: home", "~ /home/ana/.local/share/x and /home/anabel", "~ ~/.local/share/x and /home/anabel");
    expect(&s, "short name: no path around", "ana pressed F8 / again", "ana pressed F8 / again");

    PrivTable h;
    priv_clear(&h, 1);
    priv_add(&h, "C:\\Users\\Ana", PRIV_PREFIX, "%USERPROFILE%");
    int hosts = priv_add(&h, "ANA-LAPTOP", PRIV_HOST, "%COMPUTERNAME%") + priv_add(&h, "localhost", PRIV_HOST, "%COMPUTERNAME%") +
                priv_add(&h, "pc", PRIV_HOST, "%COMPUTERNAME%");
    expect(&h, "host: a UNC path", "\\\\ana-laptop\\jogos\\oot\\x.log", "\\\\%COMPUTERNAME%\\jogos\\oot\\x.log");
    expect(&h, "host: in text, any case", "net: Ana-Laptop.local answered", "net: %COMPUTERNAME%.local answered");
    expect(&h, "host: inside another word", "ana-laptops xana-laptop", "ana-laptops xana-laptop");
    priv_clear(&h, 0);
    priv_add(&h, "maria-pc", PRIV_HOST, "%COMPUTERNAME%");
    expect(&h, "host: Linux, no case either", "pulse: MARIA-PC/x and maria-pc", "pulse: %COMPUTERNAME%/x and %COMPUTERNAME%");

    PrivTable c;
    priv_clear(&c, 0);
    int common = priv_add(&c, "user", PRIV_NAME, "$USER") + priv_add(&c, "Deck", PRIV_NAME, "$USER");
    priv_add(&c, "/home/deck", PRIV_PREFIX, "~");
    expect(&c, "common name: home still short", "/home/deck/x /run/user/1000 /srv/deck/y", "~/x /run/user/1000 /srv/deck/y");

    PrivTable d;
    priv_clear(&d, 0);
    expect(&d, "empty table", "/home/ana/x", "/home/ana/x");
    int roots = priv_add(&d, "/", PRIV_PREFIX, "~") + priv_add(&d, "", PRIV_PREFIX, "~") + priv_add(&d, "ab", PRIV_NAME, "$USER") +
                priv_add(&d, "a/b", PRIV_NAME, "$USER");
    priv_clear(&d, 1);
    roots += priv_add(&d, "C:\\", PRIV_PREFIX, "%USERPROFILE%") + priv_add(&d, "H:", PRIV_PREFIX, "%USERPROFILE%");
    int twice = priv_add(&d, "C:\\Users\\a", PRIV_PREFIX, "%USERPROFILE%") + priv_add(&d, "c:/users/A/", PRIV_PREFIX, "%USERPROFILE%");
    int ok = roots == 0 && twice == 1 && common == 0 && hosts == 1;
    printf("roots, short names, common names and hosts, and repeats refused: %s", ok ? "ok\n" : "FAIL\n");
    if (!ok) printf("  roots added %d, twice added %d, common names added %d, hosts %d\n", roots, twice, common, hosts), bad++;

    printf("log_privacy_check: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
