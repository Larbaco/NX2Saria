// runtime/platform/linux.c plat_library_path and runtime/lib_origin.c on a real loader: this program is linked as the
// game is (-no-pie, runpath $ORIGIN) against a small library tests/library_path_check.py builds beside it, and prints
// what plat_library_path found for argv[1] and the "libs:" line it would make of it.
#include <stdio.h>

#include "lib_origin.h"
#include "platform/platform.h"

int probe_lib_value(void);   // the library's one function: keeps it a DT_NEEDED entry

int main(int argc, char **argv) {
    char lib[PLAT_PATH_MAX * 2], exe[PLAT_PATH_MAX * 2], line[2048];
    int found = argc > 1 && plat_library_path(argv[1], lib, sizeof lib);
    if (!found) lib[0] = 0;
    if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
    lib_origin_line(line, sizeof line, 3004018, 3004018, lib, exe, 0);
    printf("found=%d\npath=%s\nline=%s\n", found, lib, line);
    return probe_lib_value() == 42 ? 0 : 1;
}
