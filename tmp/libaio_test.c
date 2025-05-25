#define _GNU_SOURCE
#include <libaio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>

#define BS        (1024 * 1024)    // block size = 1MB
#define QD        32               // queue depth
#define TOTAL_SIZE (1L * 1024 * 1024 * 1024) // 100GB
#define FILENAME  "/dev/sda"

struct io_context_data {
    void *buf;
    struct iocb iocb;
};

double time_diff(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) + 
           (end.tv_usec - start.tv_usec) / 1000000.0;
}

void test_io(int is_write) {
    int fd = open(FILENAME, O_DIRECT | O_RDWR);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    io_context_t ctx = 0;
    if (io_setup(QD, &ctx)) {
        perror("io_setup");
        exit(1);
    }

    struct io_context_data *contexts = calloc(QD, sizeof(struct io_context_data));
    struct iocb *iocbs[QD];
    struct io_event events[QD];
    uint64_t submitted_blocks = 0, completed_blocks = 0;
    off_t offset = 0;

    // prepare memory
    for (int i = 0; i < QD; ++i) {
        if (posix_memalign(&contexts[i].buf, 512, BS)) {
            perror("posix_memalign");
            exit(1);
        }
        memset(contexts[i].buf, is_write ? 0xAB : 0, BS);
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    while ((submitted_blocks + QD) * BS <= TOTAL_SIZE) {
        for (int i = 0; i < QD; ++i) {
            off_t this_offset = offset + i * BS;
            if (is_write)
                io_prep_pwrite(&contexts[i].iocb, fd, contexts[i].buf, BS, this_offset);
            else
                io_prep_pread(&contexts[i].iocb, fd, contexts[i].buf, BS, this_offset);
            iocbs[i] = &contexts[i].iocb;
        }

        int ret = io_submit(ctx, QD, iocbs);
        if (ret < 0) {
            perror("io_submit");
            break;
        }
        submitted_blocks += ret;

        ret = io_getevents(ctx, QD, QD, events, NULL);
        if (ret < 0) {
            perror("io_getevents");
            break;
        }
        completed_blocks += ret;
        offset += QD * BS;
    }

    gettimeofday(&end, NULL);

    double seconds = time_diff(start, end);
    double total_MB = completed_blocks * BS / (1024.0 * 1024.0);
    printf("%s: %.2f MB/s (%.2f seconds, %ld MB transferred)\n",
           is_write ? "Write" : "Read",
           total_MB / seconds, seconds, (completed_blocks * BS) / (1024 * 1024));

    for (int i = 0; i < QD; ++i) {
        free(contexts[i].buf);
    }
    free(contexts);
    io_destroy(ctx);
    close(fd);
}

int main() {
    printf("Testing libaio performance on %s\n", FILENAME);
    test_io(1); // write test
    test_io(0); // read test
    return 0;
}

