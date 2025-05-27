#include "raidio.h"
#include <stdio.h>

// ./rio -f /dev/sda -b 1024K --ioengine libaio -q 6 --size 100M --thread_n 4 --rw read --direct=1 --raid_level=1 --rcache=0 --wcache=0 --pdcache=0 --raid_type hard
int main(int argc, char *argv[]) //todo, add char *envp[]
{
	// int ret = 1;
    // const char *file = NULL;
    // for (int i = 1; i < argc; i++) {
    //     if (strncmp(argv[i], "--file=", 7) == 0) {
    //         file = argv[i] + 7; 
    //         break;
    //     }
    // }

    struct rio_args opt;
    memset(&opt, 0, sizeof(opt));
	if (rio_parse_options(argc, argv, &opt))
		goto done;

    int full_stripe_size = 0;
    if(opt.raid_cf.raid_level == 0){
        full_stripe_size = opt.raid_cf.num_members * opt.raid_cf.strip_size;
        printf("%d disks raid%d, full stripe size :%d KB\n", opt.raid_cf.num_members, opt.raid_cf.raid_level, opt.raid_cf.num_members * opt.raid_cf.strip_size);
    } else if(opt.raid_cf.raid_level == 5){
        full_stripe_size = (opt.raid_cf.num_members - 1)  * opt.raid_cf.strip_size;
        printf("%d disks raid%d, full stripe size :%d KB\n", opt.raid_cf.num_members, opt.raid_cf.raid_level, (opt.raid_cf.num_members - 1) * opt.raid_cf.strip_size);
    } else if(opt.raid_cf.raid_level == 6){
        full_stripe_size = (opt.raid_cf.num_members - 2)  * opt.raid_cf.strip_size;
        printf("%d disks raid%d, full stripe size :%d KB\n", opt.raid_cf.num_members, opt.raid_cf.raid_level, (opt.raid_cf.num_members - 2) * opt.raid_cf.strip_size);
    }

    const int qd[5] = {1, 4, 16, 64, 256};
    const int num[4] = {1, 4, 16, 32};
    switch (opt.fk_plot) {
        case FAST_PLOT_BW: //bw_log
            printf("处理带宽绘图逻辑, seq big_bs num qd\n");
            char *rw_seq[] = {"read", "write"};
            int bs_seq[5] = {64, 128, 256, 1024, 0};
            bs_seq[4] = full_stripe_size;
            for(int i=0 ; i < 2 ; ++i ){ //顺序读写
                opt.rw_type = rw_seq[i];
                for(int j = 0 ; j < 4 ; ++j){//num
                    opt.thread_n = num[j];
                    for(int k = 0 ; k < 5 ; ++k){ //qd
                        opt.iodepth = qd[k];
                        for(int m = 0 ; m < 5 ; ++m){ //bs
                            opt.block_size = bs_seq[m] * 1024;
                            libaio_run(&opt);
                        }
                    }
                }
            }
            goto done;
        case FAST_PLOT_IOPS://iops_log
            printf("处理 IOPS 绘图逻辑\n");
            char *rw[] = {"randread", "randwrite"};
            int bs[3] = {4, 16, 32};
            for(int i=0 ; i < 2 ; ++i ){ //随机读写
                opt.rw_type = rw[i];
                for(int j = 0 ; j < 4 ; ++j){//num
                    opt.thread_n = num[j];
                    for(int k = 0 ; k < 5 ; ++k){ //qd
                        opt.iodepth = qd[k];
                        for(int m = 0 ; m < 3 ; ++m){ //bs
                            opt.block_size = bs[m] * 1024;
                            if(libaio_run(&opt)){
                                printf("libaio read/write error...\n");
                            }
                            plot();
                        }
                    }
                }
            }
            goto done;
        case FAST_PLOT_TAILLAT://lat_log
            // 执行尾延迟相关逻辑
            printf("处理尾延迟绘图逻辑\n");
            goto done;
        default:
            // 如果不是任何已知的枚举值，处理异常或报错
            goto normal;
    }

normal:      
    if (strcmp(opt.rw_type, "read") != 0 &&
        strcmp(opt.rw_type, "randread") != 0 &&
        strcmp(opt.rw_type, "write") != 0 &&
        strcmp(opt.rw_type, "randwrite") != 0) {
        fprintf(stderr, "Error: Unsupported rw_type: '%s'. Must be one of: read, randread, write, randwrite.\n", opt.rw_type);
        exit(EXIT_FAILURE);
    } else{
        printf("[1] Start %s task call...\n",opt.rw_type);
        if(libaio_run(&opt)){
            printf("libaio read/write error...\n");
        }
    }


done:
	clean_rio();
	return 0;
}