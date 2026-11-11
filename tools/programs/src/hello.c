#include <stdio.h>
int main(int argc, char** argv) {
    printf("hello argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "-");
    fprintf(stderr, "stderr ok\n");
    return 7;
}
