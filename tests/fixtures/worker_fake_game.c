#include <windows.h>
#include <stdio.h>
int main(void) {
    Sleep(1000);
    FILE *f = fopen("game-finished", "w");
    if (!f) return 1;
    fputs("done", f);
    fclose(f);
    return 0;
}
