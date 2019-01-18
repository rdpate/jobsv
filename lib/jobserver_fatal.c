#include "jobserver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const *prefix;
void jobserver_error_set_cmd(char const *cmd) {
    char const *s = strrchr(cmd, '/');
    if (s && s[1]) prefix = s + 1;
    else prefix = cmd;
    }
void jobserver_error(int exitcode, char const *func, int errnum, char const *msg) {
    if (!prefix) prefix = "[unknown]";
    if (!exitcode) {
        if (!msg) msg = strerror(errnum);
        if (!msg) {
            if (func) fprintf(stderr, "%s: %s\n", prefix, func);
            // else no exitcode, no func, no errnum, no msg
            }
        else if (func) fprintf(stderr, "%s: (%s) %s\n", prefix, func, msg);
        else fprintf(stderr, "%s: %s\n", prefix, msg);
        return;
        }

    if (errnum <= 0) {
        if (!msg) msg = "unknown";
        if (!func) fprintf(stderr, "%s error: %s\n", prefix, msg);
        else fprintf(stderr, "%s error: %s: %s\n", prefix, func, msg);
        }
    else {
        if (!msg) msg = strerror(errnum);
        if (!func) {
            fprintf(stderr, "%s error: %s [errno %d]\n", prefix, msg, errnum);
            }
        else {
            fprintf(stderr, "%s error: %s: %s [errno %d]\n",
                prefix, func, msg, errnum);
            }
        }
    exit(exitcode);
    }
