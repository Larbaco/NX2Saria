#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/platform/commit_error.h"

int main(void) {
    const char *advice = "Windows ran out of memory it can promise (RAM plus page file). Close memory-heavy programs "
                         "and turn on or enlarge the page file (32 GB fixed on a 16 GB PC, 32768 MB), then restart "
                         "Windows";
    assert(!strcmp(plat_commit_win_memory_advice(1455), advice));   // ERROR_COMMITMENT_LIMIT
    assert(!strcmp(plat_commit_win_memory_advice(8), advice));      // ERROR_NOT_ENOUGH_MEMORY
    assert(!strcmp(plat_commit_win_memory_advice(1450), advice));   // ERROR_NO_SYSTEM_RESOURCES
    assert(!plat_commit_win_memory_advice(14));                    // ERROR_OUTOFMEMORY
    assert(!plat_commit_win_memory_advice(487));                    // ERROR_INVALID_ADDRESS
    assert(!plat_commit_win_memory_advice(0));
    return 0;
}
