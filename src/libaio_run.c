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

typedef struct {
    int id;
    const char *file; // sda
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

    if(DEBUG) printf("@@@@  direct : %d\n",ctx->direct);
    int fd = open(filepath, O_RDWR | (ctx->direct ? O_DIRECT : 0));
    if (fd < 0) {
        perror("open");return 1;
    }

    struct iocb *iocbs[ctx->qd];
    struct io_event events[ctx->qd];
    struct {
        void *buf;
        struct iocb iocb;
    } reqs[ctx->qd];
    
    for (int i = 0; i < ctx->qd; ++i) {
        if (posix_memalign(&reqs[i].buf, 512, ctx->bs)) {
            perror("posix_memalign");
            exit(1);
        }
        memset(reqs[i].buf, (strcmp(ctx->rw, "write") == 0 || strcmp(ctx->rw, "randwrite") == 0) ? 0xAB : 0, ctx->bs);
    }

    u_int64_t offset = ctx->offset_start;
    u_int64_t submitteds = 0;
    u_int64_t completeds = 0;

    u_int64_t nr_requests = ctx->size / ctx->bs;
    
    //double total_elapsed_ms = 0;
    double elapsed_ms = 0;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    while(nr_requests > submitteds){

        u_int64_t ios_to_submit = (nr_requests - submitteds) < ctx->qd ? (nr_requests - submitteds) : ctx->qd;
        for (int i = 0; i < ios_to_submit; ++i) {
            u_int64_t off = offset +i * ctx->bs;
            if (strcmp(ctx->rw, "read") == 0 || strcmp(ctx->rw, "randread") == 0){
                io_prep_pread(&reqs[i].iocb, fd, reqs[i].buf, ctx->bs, off);
            } else{
                io_prep_pwrite(&reqs[i].iocb, fd, reqs[i].buf, ctx->bs, off);
            }
            iocbs[i] = &reqs[i].iocb;
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
        completeds += ret;
        offset += ios_to_submit * ctx->bs;
        //if(DEBUG) printf("In this submit, ios_to_submit: %lld, ctx->qd: %d, ",ios_to_submit,ctx->qd);   
    }
    gettimeofday(&end, NULL);
    long start_us = start.tv_sec * 1000000 + start.tv_usec;
    long end_us = end.tv_sec * 1000000 + end.tv_usec;
    elapsed_ms = (end_us - start_us) / 1000.0;

    //total_elapsed_ms += elapsed_ms;
   
    ctx->submitted = submitteds;
    ctx->completed = completeds;
    ctx->elapsed_ms = elapsed_ms;

    io_destroy(ioc);
    close(fd);
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
        contexts[i].file = args->file;
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

    if(DEBUG){
        double mb = total_io * args->block_size / 1024.0 / 1024.0;
        printf("\n== %s Summary ==\n", args->rw_type);
        printf("Threads    : %d\n", num_threads);
        printf("Total MB   : %.2f MB\n", mb);
        printf("Max Time   : %.2f sec\n", total_time/1000);
        printf("Total BW   : %.2f MB/s\n\n", mb / max_time * 1000);
    }

    // double toutal_size = (double)total_io*bs/1024/1024;
    // printf("Total completed IO: %d, block size: %d, IO_SIZE=%.2f MB\n", total_io, bs,toutal_size);
    // printf("Max time: %.2f ms (max of threads)\n", max_time);
    // printf("Aggregate IOPS: %.2f, Aggregate BW: %.2f MB/s\n", total_io / (max_time / 1000.0), toutal_size/max_time*1000);

    free(threads);
    free(contexts);
    
    return 0;
}