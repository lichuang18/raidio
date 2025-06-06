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
    int i;
    u_int64_t nr_requests = ctx->size / ctx->bs;
    double start = get_time_usec();
    while (nr_requests > ctx->submitted) {
        int index = ctx->submitted % ctx->qd;
        struct iocb *iocb = &ctx->iocbs[index];

        io_prep_pwrite(iocb, ctx->fd, ctx->buffers[index], ctx->bs, offset);
        ctx->iocb_ptrs[index] = iocb;
        io_submit(ctx->ctx, 1, &ctx->iocb_ptrs[index]);
        ctx->submitted++;
        offset += ctx->bs * 1024;//

        if (ctx->submitted % ctx->qd == 0) {
            int ret = io_getevents(ctx->ctx, 1, ctx->qd, ctx->events, NULL);
            for (i = 0; i < ret; i++)
                ctx->bytes_done += ctx->events[i].res;
        }
    }

    // 收尾：收集所有未完成请求
    if (ctx->submitted % ctx->qd != 0) {
        int remaining = ctx->submitted % ctx->qd;
        int ret = io_getevents(ctx->ctx, remaining, remaining, ctx->events, NULL);
        for (i = 0; i < ret; i++)
            ctx->bytes_done += ctx->events[i].res;
    }
    double end = get_time_usec();
    ctx->elapsed_us = end - start; // 转换为毫秒
    return NULL;
}

// void *io_thread(void *arg) {
//     thread_ctx_t *ctx = (thread_ctx_t *)arg;
//     u_int64_t offset = 0;//ctx->offset_start;
//     struct timespec start, end;
//     u_int64_t nr_requests = ctx->size / ctx->bs;
//     srand(time(NULL) + pthread_self());

//     clock_gettime(CLOCK_REALTIME, &start);
//     while(nr_requests > ctx->submitted){ //还有请求未提交就继续执行
//         // u_int64_t blk_index;
//         // if (strcmp(ctx->rw, "randread") == 0 || strcmp(ctx->rw, "randwrite") == 0) {
//         //     blk_index = rand() % (nr_requests);
//         // } else {//顺序读写
//         //     blk_index = ctx->submitted + 1;
//         // }
//         // u_int64_t offset = ctx->offset_start + blk_index * ctx->bs;
//         int index = ctx->submitted % ctx->qd ;
//         struct iocb *iocb = &ctx->iocbs[index];

//         if (strcmp(ctx->rw, "read") == 0 || strcmp(ctx->rw, "randread") == 0){
//             io_prep_pread(iocb, ctx->fd, ctx->buffers[index], ctx->bs, offset);
//         } else{
//             io_prep_pwrite(iocb, ctx->fd, ctx->buffers[index], ctx->bs, offset);
//         }
//         ctx->iocb_ptrs[index] = iocb;
//         int ret = io_submit(ctx->ctx, 1, &ctx->iocb_ptrs[index]);

//         if (ret == 1) {
//             ctx->submitted++;
//         } else if (ret < 0) {
//             perror("io_submit failed");
//             break;
//         }
//         if (ctx->submitted % ctx->qd == 0) {
//             int ret = io_getevents(ctx->ctx, 1, ctx->qd, ctx->events, NULL);
//             for (int i = 0; i < ret; i++)
//                 ctx->bytes_done += ctx->events[i].res;
//         }
//     }

//     if (ctx->submitted % ctx->qd != 0) { // 收尾：收集所有未完成请求
//         int remaining = ctx->submitted % ctx->qd;
//         int ret = io_getevents(ctx->ctx, 1, remaining, ctx->events, NULL);
//         for (int i = 0; i < ret; i++){
//             ctx->bytes_done += ctx->events[i].res;
//         }
//     } 
//         // if(LOG_LAT){
//         //     for (int i = 0; i < ret; ++i) {
//         //         struct iocb *cb = events[i].obj;
//         //         // 找到对应 req 的索引
//         //         int req_idx = -1;
//         //         for (size_t j = 0; j < ios_to_submit; ++j) {
//         //             if (&reqs[j].iocb == cb) {
//         //                 req_idx = j;
//         //                 break;
//         //             }
//         //         }
//         //         if (req_idx != -1) {
//         //             struct timespec done_ts;
//         //             clock_gettime(CLOCK_REALTIME, &done_ts);
//         //             uint64_t start_ns = reqs[req_idx].submit_ts.tv_sec * 1000000000ULL + reqs[req_idx].submit_ts.tv_nsec;
//         //             uint64_t end_ns = done_ts.tv_sec * 1000000000ULL + done_ts.tv_nsec;
//         //             uint64_t latency_ns = end_ns - start_ns;
//         //             int bucket = latency_ns / 1000; //每 1us 一个桶
//         //             if (bucket >= BUCKETS) bucket = BUCKETS - 1;
//         //             ctx->latency_hist[bucket]++;
//         //             ctx->total_ios++;
//         //         }
//         //     }
//         // } 
//     clock_gettime(CLOCK_REALTIME, &end);
//     u_int64_t start_ns = start.tv_sec * 1000000000L + start.tv_nsec;
//     u_int64_t end_ns = end.tv_sec * 1000000000L + end.tv_nsec;
//     ctx->elapsed_ms = (end_ns - start_ns) / 1e6; // 转换为毫秒
//     // ctx->submitted = submitteds;
//     pthread_exit(NULL);
// }


int libaio_run(struct rio_args *args) {
    u_int64_t bs = args->block_size;
    int qd = args->iodepth;
    int num_threads = args->thread_n;
    u_int64_t size = args->size;
    if(size < bs){ perror("size too small"); return 1;}
    u_int64_t per_thread_size = size / num_threads;

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    thread_ctx_t *contexts = malloc(num_threads * sizeof(thread_ctx_t));

    double start = get_time_usec();
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
        // free(contexts[i].iocbs);
        // free(contexts[i].iocb_ptrs);
        // free(contexts[i].events);
        // for (int j = 0; j < qd; j++)
        //     free(contexts[i].buffers[j]);
        // close(contexts[i].fd);
        // io_destroy(contexts[i].ctx);
    }

    double end = get_time_usec();
    long long total_bytes = contexts[0].bytes_done;// + td[1].bytes_done;
    //double elapsed_sec = (end - start) / 1e6;
    printf("Total bytes written: %" PRIu64 "\n", contexts[0].bytes_done);
    printf("Elapsed time: %.2f seconds\n", contexts[0].elapsed_us / 1e6);
    printf("Bandwidth: %.2f MB/s\n", contexts[0].bytes_done / contexts[0].elapsed_us / (1024 * 1024));

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


    // char log_file[256];
    // if (args->fk_plot == FAST_PLOT_NONE) {
    //     char log_file[256];
    //     snprintf(log_file, sizeof(log_file), "result/raid-normal-results.log");
    //     printf("\n== %s Summary  ==\n", args->rw_type);
    //     printf("Threads   : %d |  qd  : %d |  bs  : %" PRIu64 "KB\n", num_threads, qd, bs/1024);
    //     printf("Total MB  : %.4f MB\n", mb);
    //     printf("Max Time  : %.4f sec\n", max_time/1000);
    //     printf("Total BW  : %.2f MB/s\n\n", mb / max_time * 1000);
    // } else {
    //     snprintf(log_file, sizeof(log_file), "result/raid-%s-results.log", fast_plot_names[args->fk_plot]);
    // }
    // FILE *fp1 = fopen(log_file, "a");  // 追加写

    // if (fp1) {
    //     struct timespec ts;
    //     clock_gettime(CLOCK_REALTIME, &ts);  // 获取当前真实时间
    //     //fprintf(fp1, "%s, %d, %" PRIu64 " , %d, %.2f\n",args->rw_type, args->iodepth, args->block_size/1024, args->thread_n, mb / max_time * 1000);
    //     long long nanoseconds = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;//, %lld
    //     fprintf(fp1, "%s, %d, %" PRIu64 " , %d, %.2f, %lld\n",args->rw_type, args->iodepth, args->block_size/1024, args->thread_n, mb / max_time * 1000, nanoseconds);
    //     fclose(fp1);
    // }

    
    free(threads);
    free(contexts);
    
    return 0;
}