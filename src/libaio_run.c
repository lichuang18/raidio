#include "raidio.h"
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <libaio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define BUCKETS 1000  // 0~999个桶，每桶表示10us，覆盖0~9999us（9.9ms）

const char *fast_plot_names[] = {
    "bw",
    "iops",
    "taillat"
};
double get_time_usec()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e6 + tv.tv_usec;
}

typedef struct {
    int id;
    int fd;
    char file[128]; // sda
    int qd;
    u_int64_t bs;
    char *rw;  // 0=read, 1=write
    u_int64_t size;
    
    //for count
    double elapsed_us;
    int direct;
    char *log_buf;
    u_int64_t log_offset;
    int latency_hist[BUCKETS];  // 每线程自己的直方图
    int total_ios;              // 总提交的 IO 数
    // iocb
    io_context_t ctx;
    // struct iocb iocbs[QD];
    struct iocb *iocbs;
    struct iocb **iocb_ptrs;
    struct io_event *events;
    void **buffers;
    u_int64_t submitted;
    u_int64_t bytes_done;
    u_int64_t offset_start;
    struct timespec submit_ts;
} thread_ctx_t;

void *worker(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    long long offset = 0;
    u_int64_t nr_requests = ctx->size / ctx->bs;
    u_int64_t count_get = 0;
    double start = get_time_usec();
    int inflight = 0;
    while (nr_requests > ctx->submitted) {
        int index = ctx->submitted % ctx->qd;
        struct iocb *iocb = &ctx->iocbs[index];
        // printf("nr/sub %llu/%llu,inf %d,b_done %llu,offset %ld\n",nr_requests, count_get,inflight,ctx->bytes_done,offset);
        if(inflight < ctx->qd){
            io_prep_pwrite(iocb, ctx->fd, ctx->buffers[index], ctx->bs, offset);
            ctx->iocb_ptrs[index] = iocb;
            int subs = io_submit(ctx->ctx, 1, &ctx->iocb_ptrs[index]);
            if(subs > 0){
                ctx->submitted +=subs;
                inflight +=subs;
                offset += ctx->bs;
            }else{
                printf("io_submit error...\n");
            }
        } else {//if (inflight >= ctx->qd) { //
            int ret = io_getevents(ctx->ctx, 1, 1, ctx->events, NULL);
            if((ret == 1) && (long)ctx->events[0].res > 0){
                ctx->bytes_done += ctx->events[0].res;
            }
            if ((long)ctx->events[0].res < 0) {
                continue;
            }else{
                count_get++;
                --inflight;
            }
        }
    }
    int remaining = ctx->submitted - count_get;
    
    int ret = io_getevents(ctx->ctx, remaining, remaining, ctx->events, NULL);
    if(ret != remaining)
        printf("tail io_getevents error[%d]\n",ret);
    double end = get_time_usec();
    ctx->elapsed_us = end - start;
    return NULL;
}

int libaio_run(struct rio_args *args) {
    u_int64_t bs = args->block_size;
    int qd = args->iodepth;
    int num_threads = args->thread_n;
    u_int64_t size = args->size;
    if(size < bs){ perror("size too small"); return 1;}
    u_int64_t per_thread_size = size / num_threads;

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    thread_ctx_t *contexts = malloc(num_threads * sizeof(thread_ctx_t));

    for (int i = 0; i < num_threads; i++) {
        strncpy(contexts[i].file, args->file, sizeof(contexts[i].file) - 1);
        contexts[i].file[sizeof(contexts[i].file) - 1] = '\0'; // 确保结尾安全
        contexts[i].fd = open(contexts[i].file, O_RDWR | (args->direct ? O_DIRECT : 0));
        if (contexts[i].fd < 0) {
            perror("open");
            return 1;
        }    
        contexts[i].qd = qd;
        contexts[i].bs = bs;
        contexts[i].rw = args->rw_type;
        contexts[i].id = i;
        contexts[i].size = size;
        contexts[i].direct = args->direct;
        contexts[i].offset_start = i * per_thread_size; //默认offset从0开始
        contexts[i].iocbs = calloc(qd, sizeof(struct iocb));
        contexts[i].iocb_ptrs = calloc(qd, sizeof(struct iocb *));
        contexts[i].events = calloc(qd, sizeof(struct io_event));
        contexts[i].buffers = calloc(qd, sizeof(void *));
        contexts[i].submitted = 0;
        contexts[i].bytes_done = 0;
        if (!contexts[i].iocbs || !contexts[i].iocb_ptrs || !contexts[i].events || !contexts[i].buffers) {
            perror("calloc");
            exit(1);
        }
        if (io_setup(qd, &contexts[i].ctx) != 0) {
            perror("io_setup");
            return 1;
        }
        for (int j = 0; j < qd; j++) {
            if (posix_memalign(&contexts[i].buffers[j], 4096, bs)) {
                perror("posix_memalign");
                return 1;
            }
            memset(contexts[i].buffers[j], 0xab, bs);  // init pattern
            contexts[i].iocb_ptrs[j] = &contexts[i].iocbs[j];
        }
        pthread_create(&threads[i], NULL, worker, &contexts[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    u_int64_t total_bytes = 0;
    double use_time = 0, max_time = 0;
    for(int i = 0; i < num_threads; i++){
        total_bytes += contexts[i].bytes_done;// + td[1].bytes_done;
        use_time = contexts[i].elapsed_us / 1e6; //s
        if (use_time > max_time)
            max_time = use_time;
    }
    double bw_perf_mb = (double)total_bytes / max_time / (1024 * 1024);
    // printf("Total bytes written: %" PRIu64 "\n", total_bytes);
    // printf("Elapsed time: %.2f seconds\n", use_time);
    // printf("Bandwidth: %.2f MB/s\n",  bw_perf_mb);
    for (int i = 0; i < num_threads; i++) {
        // pthread_join(threads[i], NULL);
        free(contexts[i].iocbs);
        free(contexts[i].iocb_ptrs);
        free(contexts[i].events);
        for (int j = 0; j < qd; j++)
            free(contexts[i].buffers[j]);
        close(contexts[i].fd);
        io_destroy(contexts[i].ctx);
    }
   
    // u_int64_t total_io = 0;
    // double max_time = 0, total_time = 0;
    // for (int i = 0; i < num_threads; i++) {
    //     total_io += contexts[i].bytes_done;
    //     total_time += contexts[i].elapsed_ms;
    //     if (contexts[i].elapsed_ms > max_time)
    //         max_time = contexts[i].elapsed_ms;
    // }
    // double mb = (double)total_io / 1024.0 / 1024.0;

    char log_file[256];
    if (args->fk_plot == FAST_PLOT_NONE) {
        char log_file[256];
        snprintf(log_file, sizeof(log_file), "result/raid-normal-results.log");
        printf("\n========= %s Summary  =========\n", args->rw_type);
        printf("Threads: [%d] qd: [%d] bs: [%" PRIu64 "KB]\n", num_threads, qd, bs/1024);
        printf("Total bytes written: %" PRIu64 "\n", total_bytes);
        printf("Elapsed time: %.2f seconds\n", use_time);
        printf("Bandwidth: %.2f MB/s\n",  bw_perf_mb);
    } else {
        snprintf(log_file, sizeof(log_file), "result/raid-%s-results.log", fast_plot_names[args->fk_plot]);
    }
    FILE *fp1 = fopen(log_file, "a");  // 追加写
    if (fp1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);  // 获取当前真实时间
        long long nanoseconds = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;//, %lld
        fprintf(fp1, "%s, %d, %" PRIu64 " , %d, %.2f, %lld\n",args->rw_type, qd, bs/1024, args->thread_n, bw_perf_mb, nanoseconds);
        fclose(fp1);
    }
    free(threads);
    free(contexts);
    
    return 0;
}