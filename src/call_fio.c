#include "raidio.h"

int run_fio(struct rio_args *args){
    printf("TODO...   --call fio\n");
    printf("[FIO] file=%s, bs=%lld, rw=%s, ", args->file, args->block_size, args->rw_type);
	printf("ioengine=%s, size=%lld, iodepth=%d, thread_n=%d\n", args->ioengine, args->size, args->iodepth, args->thread_n);
    return 0;
}