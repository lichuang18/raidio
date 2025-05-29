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
#include <inttypes.h>  // 为 PRIu64 等宏
#define DEBUG 0
#define DEBUG_LBA 0

#define LOG_LAT 1

#define MAX_PDS 2
typedef struct {
    int eid;
    int slt;
    char cli[16];
} pd_info_t;
extern pd_info_t g_pd_list[MAX_PDS];

typedef enum {
    FAST_PLOT_NONE = -1,
    FAST_PLOT_BW,
    FAST_PLOT_IOPS,
    FAST_PLOT_TAILLAT
} fast_plots;

extern const char *fast_plot_names[];

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
    int capability;//GB
    char raid_name[64];
    char raid_status[16];
} raid_config;


struct rio_args {
	char file[256];
	uint64_t block_size;
	char *rw_type;
    const char *ioengine;
    uint64_t size;
    int iodepth;
    int thread_n;
    int direct;
    raid_config raid_cf;
    fast_plots fk_plot;
};



extern int rio_parse_options(int, char **, struct rio_args *args);
// extern int parse_jobs_ini(char *, int, int, int);
// extern int parse_cmd_line(int, char **, int);
extern int libaio_run(struct rio_args *args);
extern void plot(fast_plots fk_plot);
extern uint64_t parse_size(const char *str);


void set_raid_optl(const char *cli, int raid_level);
int get_ugood_disks(char *cli, pd_slot_t disks[], int max_disks);
int command_exists(const char *cmd);
int create_raid(char *cli, const raid_config *conf);