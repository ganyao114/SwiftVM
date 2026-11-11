#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/random.h>
#include <sys/stat.h>
int main(void) {
    const char* path = "svm-program-test.dat";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) { perror("open"); return 2; }
    const char msg[] = "hello-syscalls-0123456789";
    if (write(fd, msg, sizeof msg - 1) != sizeof msg - 1) return 3;
    lseek(fd, 0, SEEK_SET);
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    printf("read=%zd data=%s\n", n, buf);
    close(fd);
    struct stat st;
    if (stat(path, &st) == 0) printf("size=%lld mode=%o\n", (long long)st.st_size, (unsigned)st.st_mode & 0777);
    unlink(path);
    printf("pid>0=%d\n", getpid() > 0);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("monotonic_positive=%d\n", ts.tv_sec >= 0);
    unsigned char rnd[8];
    ssize_t r = getrandom(rnd, sizeof rnd, 0);
    printf("getrandom=%zd\n", r);
    return 0;
}
