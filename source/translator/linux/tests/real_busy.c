#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Slightly busier static test binary: exercises /proc/self/exe,
 * uname, file read/write, malloc/free, and assorted syscalls. */

static void print_uname(void) {
    struct utsname u;
    if (uname(&u) == 0) {
        printf("uname: %s %s %s %s\n", u.sysname, u.release, u.machine, u.nodename);
    } else {
        perror("uname");
    }
}

static void print_self_exe(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n >= 0) {
        buf[n] = '\0';
        printf("self exe: %s\n", buf);
    } else {
        perror("readlink");
    }
}

static void do_file_io(void) {
    const char *path = "/tmp/real_busy_test.txt";
    const char *msg = "real_busy file io test\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open write"); return; }
    if (write(fd, msg, strlen(msg)) < 0) perror("write");
    close(fd);

    fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open read"); return; }
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        printf("file read: %s", buf);
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        printf("file size: %lld\n", (long long)st.st_size);
    }
    unlink(path);
}

static void do_malloc(void) {
    size_t sz = 1024 * 1024;
    char *p = malloc(sz);
    if (!p) { printf("malloc failed\n"); return; }
    memset(p, 0xAB, sz);
    printf("malloc[0] = 0x%02x, malloc[end] = 0x%02x\n",
           (unsigned char)p[0], (unsigned char)p[sz - 1]);
    char *q = realloc(p, sz * 2);
    if (q) {
        printf("realloc ok\n");
        free(q);
    } else {
        free(p);
    }
}

int main(int argc, char **argv) {
    printf("real_busy: argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "(null)");
    print_uname();
    print_self_exe();
    do_file_io();
    do_malloc();
    printf("real_busy done, pid=%d\n", getpid());
    return 0;
}
