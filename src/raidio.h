#define _GNU_SOURCE
#include <sched.h>
#include <limits.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <inttypes.h>
#include <assert.h>

#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <libaio.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#define DEBUG 1
#define DEBUG_LBA 0
#define DEBUG_LAT 0

typedef struct {
    int eid;
    int slot;
} pd_slot_t;


typedef enum {
    RAID_TYPE_SOFT,
    RAID_TYPE_HARD,
    RAID_TYPE_NONE
} raid_type_t;

typedef struct {
    raid_type_t raid_type;
    int raid_level;
    int strip_size;
    int num_members;
    int wcache; //1 开启 0关闭  hard
    int rcache; //1 开启 0关闭  hard
    int pdcache; //1 开启 0关闭 hard
    int optimizer; //1 开启 0关闭 hard
    int capabilty;//GB
    char raid_name[64];
} raid_config;


struct rio_args {
	char file[256]; //必需
	uint64_t block_size; //bs
	const char *rw_type; //rw
    const char *ioengine;
    uint64_t size;
    int iodepth;
    int thread_n;
    int direct;
    raid_config raid_cf;
	// 可以根据需要扩展更多字段
};



extern int rio_parse_options(int, char **, struct rio_args *args);
// extern int parse_jobs_ini(char *, int, int, int);
// extern int parse_cmd_line(int, char **, int);
extern int libaio_run(struct rio_args *args);
extern int run_rio(struct rio_args *args);
extern void clean_rio(void);
extern uint64_t parse_size(const char *str);

int get_ugood_disks(char *cli, pd_slot_t disks[], int max_disks);
int command_exists(const char *cmd);
bool is_raid(const char *file);
int create_raid(char *cli, const raid_config *conf);