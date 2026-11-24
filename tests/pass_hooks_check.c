#include <stdio.h>

#include "vk_pass_hooks.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int light;
typedef struct { int hooks; } Grp;

static void pass_begin(Grp *g, int idx, int target) {
    if (pass_hooks_due(g != NULL, idx, g ? g->hooks : 0)) light = target;
    if (g && !idx) g->hooks = 1;
}

int main(void) {
    enum { EARLY = 1, SCENE = 2, OUTPUT = 3 };
    CHECK(pass_hooks_due(0, 0, 0), "a thread outside a group does not run the hooks");
    CHECK(pass_hooks_due(0, 0, 1) && pass_hooks_due(0, 5, 1), "outside a group the other two values count");
    CHECK(pass_hooks_due(1, 0, 0), "a group's first chunk does not run the hooks at its first pass");
    CHECK(!pass_hooks_due(1, 0, 1), "a group's first chunk runs the hooks again at a pass it begins again");
    for (int idx = 1; idx < 17; idx++)
        CHECK(!pass_hooks_due(1, idx, 0) && !pass_hooks_due(1, idx, 1), "chunk %d of a group runs the hooks", idx);

    light = 0;
    pass_begin(NULL, 0, EARLY);
    pass_begin(NULL, 0, EARLY);
    pass_begin(NULL, 0, SCENE);
    pass_begin(NULL, 0, SCENE);
    CHECK(light == SCENE, "one thread: the light target is %d, not the last scene pass's", light);

    Grp g = {0};
    light = 0;
    pass_begin(&g, 0, EARLY);
    pass_begin(&g, 2, EARLY);
    pass_begin(&g, 1, EARLY);
    pass_begin(&g, 0, EARLY);
    pass_begin(NULL, 0, SCENE);
    CHECK(light == SCENE, "a group waited for: the light target is %d", light);

    g = (Grp){0};
    light = 0;
    pass_begin(&g, 0, EARLY);
    pass_begin(NULL, 0, SCENE);
    pass_begin(&g, 0, EARLY);
    pass_begin(&g, 1, EARLY);
    CHECK(light == SCENE, "the submitting thread past a group: the light target is %d, the earlier pass's", light);

    Grp a = {0}, b = {0};
    light = 0;
    pass_begin(&a, 0, EARLY);
    pass_begin(&b, 0, SCENE);
    pass_begin(&a, 0, EARLY);
    pass_begin(NULL, 0, OUTPUT);
    pass_begin(&b, 0, SCENE);
    pass_begin(&a, 0, EARLY);
    CHECK(light == OUTPUT, "two groups out: the light target is %d", light);

    g = (Grp){0};
    light = 0;
    if (pass_hooks_due(1, 0, g.hooks)) light = EARLY;
    g.hooks = 1;
    CHECK(light == EARLY, "a group's hooks never ran");

    if (fails) printf("%d checks failed\n", fails);
    else printf("pass_hooks_check: ok\n");
    return fails != 0;
}
