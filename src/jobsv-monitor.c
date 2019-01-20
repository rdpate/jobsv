#include "common.h"
#include "get_fds.h"
#include "monitor.h"

int default_job_slots(void);
// defined in default_job_slots.c

static int const max_slots = 100;
    // must be less than pipe buffer size

static void do_monitor(int read_fd, int write_fd) {
    // Check every 60 seconds.  If a change was required, then check again in 30 seconds.
    int const no_change_delay = 60;
    int const change_delay = 30;
    int delay = no_change_delay;

    while (true) {
        sleep(delay);
        if (slot_change(read_fd, write_fd)) delay = change_delay;
        else delay = no_change_delay;
        }
    }

int main(int argc, char **argv) {
    int rc = main_no_options(&argc, &argv);
    if (rc) return rc;
    if (argc != 0) fatal(64, "unexpected arguments");

    if (!load_env_fds()) fatal(64, "could not load $JOBSERVER_FDS");
    int const write_fd = get_write_fd();
    int slots = default_job_slots();
    if (slots < 1) slots = 1;
    else if (slots > max_slots) slots = max_slots;
    { // add initial tokens
        slots --; // parent already has one
        char const *x16 = "xxxxxxxxxxxxxxxx";
        while (slots > 0) {
            int write_len = (slots < 16 ? slots : 16);
            int n;
            SYSV( n, block_write,(write_fd, x16, write_len) );
            slots -= n;
            }
        }
    int leak_warning = slots;
    if (leak_warning < INT_MAX / 2) leak_warning *= 2;
    init_monitor(slots, leak_warning);
    block_write(1, "\n", 1);
    do_monitor(get_read_fd(), write_fd);
    UNREACHABLE
    }
