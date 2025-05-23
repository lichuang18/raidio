#include <sched.h>
#include <limits.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

struct rio_args {
	const char *file; //必需
	uint64_t block_size; //bs
	const char *rw_type; //rw
    const char *ioengine;
    uint64_t size;
    int iodepth;
    int thread_n;
	// 可以根据需要扩展更多字段
};

extern int rio_parse_options(int, char **, struct rio_args *args);
// extern int parse_jobs_ini(char *, int, int, int);
// extern int parse_cmd_line(int, char **, int);
extern int run_fio(struct rio_args *args);
extern int run_rio(struct rio_args *args);
extern void clean_rio(void);



bool is_raid(const char *file);