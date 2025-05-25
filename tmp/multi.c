#define _GNU_SOURCE
#include <libaio.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>

#define BS (1024 * 1024)            // 1MB block size
#define QD 32                       // queue depth
#define TOTAL_SIZE (1L *10 * 1024 * 1024) // 1GB
#define DEVICE "/dev/sda"          // target device
#define NUMJOBS 4                  // number of threads

typedef struct {
    int tid;
    int is_write;
    off_t offset_start;
    size_t size;
    u_int64_t offset_start;
    // for summary
    double seconds;
    size_t bytes_done;
} thread_arg_t;

double time_diff(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) + 
           (end.tv_usec - start.tv_usec) / 1000000.0;
}

void *worker(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;

    io_context_t ctx = 0;
    if (io_setup(QD, &ctx) < 0) {
        perror("io_setup");
        pthread_exit(NULL);
    }

    int fd = open(DEVICE, O_DIRECT | O_RDWR);
    if (fd < 0) {
        perror("open");
        pthread_exit(NULL);
    }`

    struct iocb *iocbs[QD];
    struct io_event events[QD];
    struct {
        void *buf;
        struct iocb iocb;
    } reqs[QD];

    for (int i = 0; i < QD; ++i) {
        if (posix_memalign(&reqs[i].buf, 512, BS)) {
            perror("posix_memalign");
            exit(1);
        }
        memset(reqs[i].buf, targ->is_write ? 0xAB : 0, BS);
    }

    off_t offset = targ->offset_start;
    size_t submitted_blocks = 0, completed_blocks = 0;
    size_t total_blocks = targ->size / BS;

    struct timeval start, end;
    gettimeofday(&start, NULL);

    while (submitted_blocks < total_blocks) {
        int this_qd = (total_blocks - submitted_blocks) >= QD ? QD : (total_blocks - submitted_blocks);
        for (int i = 0; i < this_qd; ++i) {
            off_t off = offset + i * BS;
            if (targ->is_write)
                io_prep_pwrite(&reqs[i].iocb, fd, reqs[i].buf, BS, off);
            else
                io_prep_pread(&reqs[i].iocb, fd, reqs[i].buf, BS, off);
            iocbs[i] = &reqs[i].iocb;
        }

        int ret = io_submit(ctx, this_qd, iocbs);
        if (ret < 0) {
            perror("io_submit");
            break;
        }
        submitted_blocks += ret;

        ret = io_getevents(ctx, this_qd, this_qd, events, NULL);
        if (ret < 0) {
            perror("io_getevents");
            break;
        }
        completed_blocks += ret;
        offset += this_qd * BS;
    }

    gettimeofday(&end, NULL);
    double seconds = time_diff(start, end);
    size_t total_bytes = completed_blocks * BS;

    targ->seconds = seconds;
    targ->bytes_done = total_bytes;

    for (int i = 0; i < QD; ++i)
        free(reqs[i].buf);

    io_destroy(ctx);
    close(fd);
    pthread_exit(NULL);
}

void run_phase(int is_write) {
    pthread_t threads[NUMJOBS];
    thread_arg_t args[NUMJOBS];
    size_t per_thread_size = TOTAL_SIZE / NUMJOBS;

    for (int i = 0; i < NUMJOBS; ++i) {
        args[i].tid = i;
        args[i].is_write = is_write;
        args[i].offset_start = i * per_thread_size;
        args[i].size = per_thread_size;
        args[i].seconds = 0.0;
        args[i].bytes_done = 0;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < NUMJOBS; ++i) {
        pthread_join(threads[i], NULL);
    }

    double total_sec = 0.0, max_sec = 0.0;
    size_t total_bytes = 0;
    for (int i = 0; i < NUMJOBS; ++i) {
        if (args[i].seconds > max_sec)
            max_sec = args[i].seconds;
        total_bytes += args[i].bytes_done;
        total_sec += args[i].seconds;
    }

    double mb = total_bytes / 1024.0 / 1024.0;
    printf("\n== %s Summary ==\n", is_write ? "WRITE" : "READ");
    printf("Threads    : %d\n", NUMJOBS);
    printf("Total MB   : %.2f MB\n", mb);
    printf("Max Time   : %.2f sec\n", max_sec);
    printf("Total BW   : %.2f MB/s\n\n", mb / max_sec);
}

int main() {
    printf("Testing libaio performance on %s\n", DEVICE);
    run_phase(1);  // write
    run_phase(0);  // read
    return 0;
}
