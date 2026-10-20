#include "ps5bridge.h"
#include <stdio.h>
int main(void) {
    int result = pb_self_test();
    if(result) { fprintf(stderr, "Native self-test failed: %d\n", result); return 1; }
    puts("Native controller conversion, neutral release, and ABI self-tests passed.");
    return 0;
}
