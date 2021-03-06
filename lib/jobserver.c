// internal details
    #include "jobserver.h"
    #include "jobserver_nonblock_read.h"

    #include <errno.h>
    #include <limits.h>
    #include <signal.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <time.h>

    #include <fcntl.h>
    #include <poll.h>
    #include <sys/signalfd.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>

    static struct {
        int read_fd, write_fd;
        int slots_held, children;
        int devnull;

        int child_fd;
        jobserver_wait_callback wait_callback;
        void *wait_callback_data;
        pid_t waited_pid;
        pid_t monitor_pid;
        } self;
    static int const start_fd = 100;
    #define log_msg(msg)            jobserver_error(0, __func__, 0, msg)
    //#define fatal_func(func)        jobserver_error(70, func, errno, NULL)
    #define fatal_sysfunc(func)     jobserver_error(71, func, errno, NULL)
    #define fatal_msg(msg)          jobserver_error(70, __func__, 0, msg)
    #define check_init_return(rc) do { \
        if (!self.write_fd) { \
            fatal_msg("must jobserver_init first"); \
            return rc; \
            } \
        } while (0)
// High-level Interface
    static bool set_env_var(int read_fd, int write_fd) {
        if (read_fd < 0 || write_fd < 0) {
            fatal_msg("file descriptor < 0");
            return false;
            }
        if (read_fd > 99999 || write_fd > 99999) {
            fatal_msg("file descriptor > 99999");
            return false;
            }
        char buffer[5+1+5+1];
        sprintf(buffer, "%d,%d", read_fd, write_fd);
        if (setenv("JOBSERVER_FDS", buffer, true) == -1) {
            fatal_sysfunc("setenv");
            return false;
            }
        return true;
        }
    static int move_fd(int fd, int start_fd) {
        int new_fd = fcntl(fd, F_DUPFD, start_fd);
        if (new_fd != -1) {
            close(fd);
            fd = new_fd;
            }
        return fd;
        }
    static bool sync_fallback(void) {
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            fatal_sysfunc("pipe");
            return false;
            }
        pipefd[0] = move_fd(pipefd[0], start_fd);
        pipefd[1] = move_fd(pipefd[1], start_fd);
        if (!set_env_var(pipefd[0], pipefd[1])) {
            close(pipefd[0]);
            close(pipefd[1]);
            return false;
            }
        self.read_fd = pipefd[0];
        self.write_fd = pipefd[1];
        self.slots_held = 1;
        return true;
        }
    bool jobserver_init_or_monitor(void) {
        if (jobserver_init()) return true;
        if (errno != 0) return false;
        // piggy-back on sync_fallback, adding monitor once pipe is setup
        if (!sync_fallback()) return false;
        int pipefd[2];
        if (pipe(pipefd) == -1) goto sync_fallback;
        pid_t pid = fork();
        if (pid == -1) {
            fatal_sysfunc("fork");
            close(pipefd[0]);
            close(pipefd[1]);
            goto sync_fallback;
            }
        if (pid == 0) {
            // child
            close(pipefd[0]);
            dup2(pipefd[1], 1);
            close(pipefd[1]);
            char const *argv[] = {"jobsv-monitor", 0};
            execvp(argv[0], (char **)argv);
            fatal_sysfunc("execvp");
            exit(71);
            // will fallback to synchronous
            }
        close(pipefd[1]);
        while (true) {
            char c;
            errno = 0;
            ssize_t x = read(pipefd[0], &c, 1);
            if (x == 1) break;
            if (errno == EINTR) continue;
            close(pipefd[0]);
            waitpid(pid, 0, 0);
            goto sync_fallback;
            }
        self.monitor_pid = pid;
        return true;
        sync_fallback:
        log_msg("synchronous fallback");
        return false;
        }
    bool jobserver_init_or_exec(char **argv) {
        if (!argv || !*argv) {
            fatal_msg("bad argv");
            return false;
            }
        if (jobserver_init()) return true;
        if (errno != 0) return false;

        int argc = 1;
        for (char **x = argv + 1; *x; x ++) argc ++;
        char *new_argv[argc + 2 + 1];
        new_argv[0] = "jobsv";
        new_argv[1] = "--";
        char **n = new_argv + 2;
        for (char **x = argv; *x; x ++) {
            *n = *x;
            n ++;
            }
        *n = 0;
        execvp(new_argv[0], new_argv);
        if (errno == ENOENT || errno == EACCES) {
            log_msg("synchronous fallback");
            return sync_fallback();
            }
        fatal_sysfunc("exec");
        return false;
        }
    bool jobserver_init_or_sync(void) {
        if (jobserver_init()) return true;
        if (errno != 0) return false;
        return sync_fallback();
        }
    static int spawn(void *data) {
        char **args = data;
        execvp(args[0], args);
        return 71;
        }
    pid_t jobserver_bg(int (*func)(void *data), void *data) {
        check_init_return((pid_t) -1);
        pid_t pid = fork();
        if (pid == -1) {
            fatal_sysfunc("fork");
            return pid;
            }
        if (pid == 0) {
            // child
            jobserver_forked_child();
            if (!func) func = &spawn;
            _Exit(func(data));
            }
        // parent
        jobserver_forked_parent();
        jobserver_acquire_wait(0, 0);
        if (self.waited_pid == pid) pid = 0;
        return pid;
        }
    pid_t jobserver_bg_spawn(char const *cmd, char const *args, ...) {
        if (!args) {
            char const *argv[2] = {cmd, 0};
            return jobserver_bg(0, argv);
            }
        int n = 0;
        va_list ap;
        va_start(ap, args);
        while (va_arg(ap, char const *)) n ++;
        va_end(ap);
        char const *argv[n + 3];
        argv[0] = cmd;
        argv[1] = args;
        va_start(ap, args);
        char const **next = argv + 2;
        for (char const *a; (a = va_arg(ap, char const *));) {
            *next = a;
            next ++;
            }
        *next = NULL;
        va_end(ap);
        return jobserver_bg(0, argv);
        }
    pid_t jobserver_bg_shell(char const *script, char const *args, ...) {
        char const *shell = getenv("SHELL");
        if (!shell) {
            fatal_sysfunc("getenv");
            return -1;
            }
        if (!args) {
            char const *argv[5] = {shell, "-uec", script, shell, 0};
            return jobserver_bg(0, argv);
            }
        int n = 0;
        va_list ap;
        va_start(ap, args);
        while (va_arg(ap, char const *)) n ++;
        va_end(ap);
        char const *argv[n + 6];
        argv[0] = shell;
        argv[1] = "-Cuec";
        argv[2] = script;
        argv[3] = shell;
        argv[4] = args;
        va_start(ap, args);
        char const **next = argv + 5;
        for (char const *a; (a = va_arg(ap, char const *));) {
            *next = a;
            next ++;
            }
        *next = NULL;
        va_end(ap);
        return jobserver_bg(0, argv);
        }
    bool jobserver_exiting(void) {
        check_init_return(false);
        if (self.slots_held < 0) {
            fatal_msg("too many slots released");
            return false;
            }
        if (self.children < 0) {
            fatal_msg("too many children reaped");
            }
        while (self.children > 0) {
            if (wait(0) == -1) {
                if (errno == EINTR) continue;
                fatal_sysfunc("wait");
                return false;
                }
            if (!jobserver_waited_keep(self.children == 1))
                return false;
            }
        if (!jobserver_acquire_wait(0, 0)) return false;
        while (self.slots_held > 1)
            if (!jobserver_release_keep(1)) return false;
        if (self.monitor_pid) kill(self.monitor_pid, SIGHUP);
        return true;
        }
// Manual Forking
    void jobserver_forked_parent(void) {
        self.slots_held --;
        self.children ++;
        }
    void jobserver_forked_child(void) {
        self.slots_held = 1;
        self.children = 0;
        if (dup2(self.devnull, 0) == -1) {
            _Exit(71);
            }
        self.devnull = 0;
        }
    bool jobserver_waited(void) {
        self.children --;
        self.slots_held ++;
        return jobserver_release_keep(0);
        }
    bool jobserver_waited_keep(int keep_slots) {
        self.children --;
        self.slots_held ++;
        if (self.slots_held <= keep_slots)
            return true;
        return jobserver_release_keep(keep_slots);
        }
// Low-level Interface
    bool jobserver_init(void) {
        if (self.write_fd) return true;
        // write_fd starts as 0, but never 0 after success here

        if (!jobserver_nonblock_read_init()) return false;

        // first, initialization which can be "shared" with init_or_sync
        // TODO: instead, set a flag to no-op (almost) everthing?
        if (!self.devnull) {
            if ((self.devnull = open("/dev/null", O_RDONLY | O_CLOEXEC)) == -1) {
                fatal_sysfunc("open");
                return false;
                }
            }
        if (!self.child_fd) {
            sigset_t mask;
            if (sigemptyset(&mask) == -1) {
                fatal_sysfunc("sigemptyset");
                return false;
                }
            if (sigaddset(&mask, SIGCHLD) == -1) {
                fatal_sysfunc("sigaddset");
                return false;
                }
            int fd = signalfd(-1, &mask, SFD_CLOEXEC);
            if (fd == -1) {
                fatal_sysfunc("signalfd");
                return false;
                }
            // TODO: inherit sigprocmask?
            if (sigprocmask(SIG_BLOCK, &mask, 0) == -1) {
                fatal_sysfunc("sigprocmask");
                return false;
                }
            self.child_fd = fd;
            }
        long read_fd, write_fd; {
            errno = 0;
            char const *env = getenv("JOBSERVER_FDS");
            if (!env || !*env) goto invalid_env;
            char const *rest;
            read_fd = strtol(env, (char **) &rest, 10);
            if (*rest != ',') goto invalid_env;
            write_fd = strtol(rest + 1, (char **) &rest, 10);
            if (*rest != '\0') goto invalid_env;
            if (read_fd  < 0 || INT_MAX < read_fd
            ||  write_fd < 0 || INT_MAX < write_fd
            ||  write_fd == 0) {
                goto invalid_env;
                }
            int fd;
            fd = dup(read_fd);
            if (fd == -1) goto invalid_env;
            close(fd);
            fd = dup(write_fd);
            if (fd == -1) goto invalid_env;
            close(fd);
            }
        self.read_fd = (int) read_fd;
        // write_fd is sentinel and assigned last
        self.slots_held = 1;  // start with one

        self.write_fd = (int) write_fd;
        return true;
        invalid_env:
        errno = 0;
        return false;
        }
    bool jobserver_set_wait_callback(jobserver_wait_callback func, void *data) {
        self.wait_callback = func;
        self.wait_callback_data = data;
        return true;
        }
    int jobserver_ready_fd(void) {
        check_init_return(-1);
        return self.read_fd;
        }
    int jobserver_child_fd(void) {
        check_init_return(-1);
        return self.child_fd;
        }
    static bool acquire(void) {
        char c;
        switch (jobserver_nonblock_read(self.read_fd, &c, 1)) {
            case -1:
                if (errno == EINTR
                ||  errno == EAGAIN
                ||  errno == EWOULDBLOCK) return false;
                fatal_sysfunc("read");
                return false;
            case 0:
                fatal_msg("unexpected EOF");
                return false;
            case 1:
            default:
                self.slots_held ++;
                return true;
            }
        }
    bool jobserver_try_acquire(void) {
        // try to own a slot; returns true if acquired or false on error
        check_init_return(false);
        if (self.slots_held > 0) return true;
        return acquire();
        }
    bool jobserver_acquire_wait(jobserver_wait_callback func, void *data) {
        check_init_return(false);
        self.waited_pid = 0;
        if (self.slots_held > 0) return true;
        struct pollfd x[2] = {{self.read_fd, POLLIN}, {self.child_fd, POLLIN}};
        while (true) {
            int n = poll(x, 2, -1);
            if (n == -1) {
                if (errno == EINTR) continue;
                fatal_sysfunc("poll");
                return false;
                }
            if (n == 0) continue;
            if (x[1].revents) {
                // child_fd
                pid_t pid;
                int wstatus;
                if ((pid = waitpid(-1, &wstatus, WNOHANG)) == -1) {
                    if (errno == ECHILD) continue;
                    fatal_sysfunc("waitpid");
                    return false;
                    }
                if (!pid) continue;
                self.waited_pid = pid;
                (void) jobserver_waited_keep(1);
                if (!func) func = self.wait_callback;
                if (func) {
                    if (!data) data = self.wait_callback_data;
                    if (!func(pid, wstatus, data)) return false;
                    }
                return true;
                }
            if (x[0].revents) {
                if (acquire()) return true;
                }
            }
        }
    bool jobserver_release_keep(int keep_slots) {
        if (keep_slots < 0) fatal_msg("keep_slots < 0");
        check_init_return(false);
        while (self.slots_held > keep_slots) {
            ssize_t n;
            int write_len = self.slots_held - keep_slots;
            if (write_len > 16) write_len = 16;;
            while ((n = write(self.write_fd, "xxxxxxxxxxxxxxxx", write_len)) == -1) {
                if (errno == EINTR) continue;
                fatal_sysfunc("write");
                return false;
                }
            if (n == 0) {
                jobserver_error(70, "write", 0, "unexpected zero-length success");
                return false;
                }
            self.slots_held -= n;
            }
        return true;
        }
