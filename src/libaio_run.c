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

typedef struct {
    int id;
    char file[128]; // sda
    int qd;
    u_int64_t bs;
    char *rw;  // 0=read, 1=write
    u_int64_t size;
    // char *buf;
    u_int64_t submitted;
    u_int64_t completed;
    u_int64_t offset_start;
    //for count
    double elapsed_ms;
    int direct;
    char *log_buf;
    u_int64_t log_offset;
    int latency_hist[BUCKETS];  // 每线程自己的直方图
    int total_ios;              // 总提交的 IO 数
} thread_ctx_t;

void *io_thread(void *arg) {
    thread_ctx_t *ctx = (thread_ctx_t *)arg;

    io_context_t ioc = 0;
    if (io_setup(ctx->qd, &ioc) < 0) {
        perror("io_setup");
        pthread_exit(NULL);
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s", ctx->file);

    if(DEBUG) printf("@@@@  direct : %d,file:%s\n",ctx->direct,ctx->file);
    int fd = open(filepath, O_RDWR | (ctx->direct ? O_DIRECT : 0));
    if (fd < 0) {
        perror("open");return NULL;
    }

    struct iocb *iocbs[ctx->qd];
    struct io_event events[ctx->qd];
    struct {
        void *buf;
        struct iocb iocb;
        struct timespec submit_ts;
    } reqs[ctx->qd];
    
    // 1. 分配一块大内存，满足对齐要求（例如 O_DIRECT 要求 4096 对齐）
    void *big_buf = NULL;
    if (posix_memalign(&big_buf, 4096, ctx->qd * ctx->bs) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    // 2. 切分大内存，赋值给每个 reqs[i].buf
    for (int i = 0; i < ctx->qd; ++i) {
        reqs[i].buf = (char *)big_buf + i * ctx->bs;
        memset(reqs[i].buf, (strcmp(ctx->rw, "write") == 0 || strcmp(ctx->rw, "randwrite") == 0) ? 0xAB : 0, ctx->bs);
    }

    // for (int i = 0; i < ctx->qd; ++i) {
    //     if (posix_memalign(&reqs[i].buf, 4096, ctx->bs)) {
    //         perror("posix_memalign");
    //         exit(1);
    //     }
    //     for (size_t j = 0; j < ctx->bs; j++) {
    //         ((char *)reqs[i].buf)[j] = rand() % 256;
    //     }
    //     // memset(reqs[i].buf, (strcmp(ctx->rw, "write") == 0 || strcmp(ctx->rw, "randwrite") == 0) ? 0xAB : 0, ctx->bs);
    // }

    //u_int64_t offset = ctx->offset_start;
    u_int64_t submitteds = 0, completeds = 0;
    srand(time(NULL) + pthread_self());
    u_int64_t nr_requests = ctx->size / ctx->bs;
    double elapsed_ms = 0;
    //struct timeval start, end;
    struct timespec log_record;
    struct timespec start, end;
    // gettimeofday(&start, NULL);
    clock_gettime(CLOCK_REALTIME, &start);
    while(nr_requests > submitteds){
        u_int64_t ios_to_submit = (nr_requests - submitteds) < (u_int64_t)ctx->qd ? (nr_requests - submitteds) : (u_int64_t)ctx->qd;
        for (size_t i = 0; i < ios_to_submit; ++i) {
            u_int64_t blk_index;
            if (strcmp(ctx->rw, "randread") == 0 || strcmp(ctx->rw, "randwrite") == 0) {
                blk_index = rand() % (nr_requests);
            } else {
                blk_index = submitteds + i;
            }
            u_int64_t offset = ctx->offset_start + blk_index * ctx->bs;

            if (strcmp(ctx->rw, "read") == 0 || strcmp(ctx->rw, "randread") == 0){
                io_prep_pread(&reqs[i].iocb, fd, reqs[i].buf, ctx->bs, offset);
            } else{
                io_prep_pwrite(&reqs[i].iocb, fd, reqs[i].buf, ctx->bs, offset);
            }
            if(LOG_LAT) clock_gettime(CLOCK_REALTIME, &reqs[i].submit_ts);
            iocbs[i] = &reqs[i].iocb;
            if(DEBUG_LBA){
                clock_gettime(CLOCK_REALTIME, &log_record);  
                // printf("Submit | Time: %ld.%09ld | Thread %lu: Submit IO #%lu to LBA: %lu (offset=%lu)\n",
                //     log_record.tv_sec, log_record.tv_nsec,pthread_self(), submitteds + i, offset / 512, offset);
            }
        }
        
        int submitted = io_submit(ioc, ios_to_submit, iocbs);
        if (submitted < 0) {
            perror("io_submit");
            pthread_exit(NULL);
        }
        submitteds += submitted; 

        int ret = io_getevents(ioc, ios_to_submit, ios_to_submit, events, NULL);
        if (ret < 0) {
            perror("io_getevents");
        }
        if(LOG_LAT){
            for (int i = 0; i < ret; ++i) {
                struct iocb *cb = events[i].obj;
                // 找到对应 req 的索引
                int req_idx = -1;
                for (size_t j = 0; j < ios_to_submit; ++j) {
                    if (&reqs[j].iocb == cb) {
                        req_idx = j;
                        break;
                    }
                }

                if (req_idx != -1) {
                    struct timespec done_ts;
                    clock_gettime(CLOCK_REALTIME, &done_ts);

                    // long start_us = reqs[req_idx].submit_ts.tv_sec * 1000000 + reqs[req_idx].submit_ts.tv_nsec / 1000;
                    // long end_us = done_ts.tv_sec * 1000000 + done_ts.tv_nsec / 1000;
                    // long latency_us = end_us - start_us;
                    uint64_t start_ns = reqs[req_idx].submit_ts.tv_sec * 1000000000ULL + reqs[req_idx].submit_ts.tv_nsec;
                    uint64_t end_ns = done_ts.tv_sec * 1000000000ULL + done_ts.tv_nsec;
                    uint64_t latency_ns = end_ns - start_ns;
                    int bucket = latency_ns / 1000; //每 1us 一个桶
                    if (bucket >= BUCKETS) bucket = BUCKETS - 1;
                    ctx->latency_hist[bucket]++;
                    ctx->total_ios++;
                    // printf("Complete | Time: %ld.%06ld | Thread %lu | IO#%lu | Latency: %ld us\n",
                    //     done_ts.tv_sec, done_ts.tv_nsec / 1000,
                    //     pthread_self(), completeds + i, latency_us);
                }
            }
        }
        completeds += ret;   
    }
    // gettimeofday(&end, NULL);
    // long start_us = start.tv_sec * 1000000 + start.tv_usec;
    // long end_us = end.tv_sec * 1000000 + end.tv_usec;
    // elapsed_ms = (end_us - start_us) / 1000.0;
    clock_gettime(CLOCK_REALTIME, &end);
    // long start_us = start.tv_sec * 1000000 + start.tv_usec;
    // long end_us = end.tv_sec * 1000000 + end.tv_usec;
    // elapsed_ms = (end_us - start_us) / 1000.0;
    u_int64_t start_ns = start.tv_sec * 1000000000L + start.tv_nsec;
    u_int64_t end_ns = end.tv_sec * 1000000000L + end.tv_nsec;
    // u_int64_t elas_ns = end_ns - start_ns ;
    elapsed_ms = (end_ns - start_ns) / 1e6; // 转换为毫秒
    //total_elapsed_ms += elapsed_ms;
   
    ctx->submitted = submitteds;
    ctx->completed = completeds;
    ctx->elapsed_ms = elapsed_ms;

    io_destroy(ioc);
    close(fd);

    // for (int i = 0; i < ctx->qd; ++i) {// 线程内释放资源
    //     free(reqs[i].buf);   
    // }
    free(big_buf);
    pthread_exit(NULL);
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
        //contexts[i].file = args->file;
        strncpy(contexts[i].file, args->file, sizeof(contexts[i].file) - 1);
        contexts[i].file[sizeof(contexts[i].file) - 1] = '\0'; // 确保结尾安全
        contexts[i].qd = qd;
        contexts[i].bs = args->block_size;
        contexts[i].rw = args->rw_type;
        contexts[i].id = i;
        contexts[i].size = size;
        contexts[i].direct = args->direct;
        contexts[i].offset_start = i * per_thread_size;
        // contexts[i].buf = aligned_alloc(4096, bs * qd);
        // if (!contexts[i].buf) {
        //     perror("aligned_alloc");
        //     return 1;
        // }
        // memset(contexts[i].buf, 0, bs * qd);
        pthread_create(&threads[i], NULL, io_thread, &contexts[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    int total_io = 0;
    double max_time = 0, total_time = 0;
    
    for (int i = 0; i < num_threads; i++) {
        total_io += contexts[i].completed;
        total_time += contexts[i].elapsed_ms;
        if (contexts[i].elapsed_ms > max_time)
            max_time = contexts[i].elapsed_ms;
    }
    double mb = (double)total_io * args->block_size / 1024.0 / 1024.0;
    if(DEBUG){
        printf("\n== %s Summary  ==\n", args->rw_type);
        printf("Threads   : %d\n", num_threads);
        printf("qd        : %d\n", qd);
        printf("Total MB  : %.2f MB(ios: %d, bs: %" PRIu64 "KB)\n", mb, total_io, args->block_size/1024);
        printf("Max Time  : %.2f sec\n", total_time/1000);
        printf("Total BW  : %.2f MB/s\n\n", mb / max_time * 1000);
    }
    char log_file[256];
    snprintf(log_file, sizeof(log_file), "result/raid-%s-results.log", fast_plot_names[args->fk_plot]);
    FILE *fp1 = fopen(log_file, "a");  // 追加写

    if (fp1) {
        fprintf(fp1, "%s, %d, %" PRIu64 " , %d, %.2f\n",args->rw_type, args->iodepth, args->block_size/1024, args->thread_n, mb / max_time * 1000);
        fclose(fp1);
    }


    free(threads);
    free(contexts);
    
    return 0;
}