#include <stdio.h>

#include "lib_origin.h"
#include "platform/platform.h"

int probe_lib_value(void);

int main(int argc, char **argv) {
    char lib[PLAT_PATH_MAX * 2], exe[PLAT_PATH_MAX * 2], line[2048];
    int found = argc > 1 && plat_library_path(argv[1], lib, sizeof lib);
    if (!found) lib[0] = 0;
    if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
    lib_origin_line(line, sizeof line, 3004018, 3004018, lib, exe, 0);
    printf("found=%d\npath=%s\nline=%s\n", found, lib, line);
    return probe_lib_value() == 42 ? 0 : 1;
}
