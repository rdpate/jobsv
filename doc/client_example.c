#include "../lib/jobserver.h"

char const *commands[] = {
    "sleep 0.6; echo abc",
    "sleep 0.3; echo def",
    "sleep 0.5; echo ghi",
    "sleep 0.4; echo jkl",
    "sleep 0.1; echo mno",
    "sleep 0.2; echo xyz",
    0};

int main(int argc, char **argv) {
    jobserver_error_set_cmd(argv[0]);
    jobserver_init_or_monitor();

    for (char const **x = commands; *x; x ++) {
        jobserver_bg_shell(*x, 0);
        }

    jobserver_exiting();
    return 0;
    }
