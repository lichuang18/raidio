#include "raidio.h"
#include <stdio.h>

// ./rio -f /dev/sda -b 1024K --ioengine libaio -q 6 --size 100M --thread_n 4 --rw read --direct=1 --raid_level=1 --rcache=0 --wcache=0 --pdcache=0 --raid_type hard
int main(int argc, char *argv[]) //todo, add char *envp[]
{
    struct rio_args opt;
    memset(&opt, 0, sizeof(opt));
	if (rio_parse_options(argc, argv, &opt))
		goto done;


    int full_stripe_size = 0;

    switch (opt.raid_cf.raid_type) {
        case RAID_TYPE_HARD:
        case RAID_TYPE_SOFT:
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
            break;
        case RAID_TYPE_NONE:
            goto normal;
        default:
            goto done;
    }

    const int qd[] = {1, 4, 16, 64, 256};
    size_t qd_len = sizeof(qd) / sizeof(qd[0]);
    const int num[] = {1, 4, 16, 32};
    size_t num_len = sizeof(num) / sizeof(num[0]);
    switch (opt.fk_plot) {
        case FAST_PLOT_BW: //bw_log
            printf("处理带宽绘图逻辑, seq big_bs num qd\n");
            char *rw_seq[] = {"read", "write"};
            int bs_seq[] = {64, 128, 256, 1024, 0};
            // opt.size = 1024ULL * 1024 * 1024; //1G
            size_t bs_seq_len = sizeof(bs_seq) / sizeof(bs_seq[0]);
            bs_seq[bs_seq_len-1] = full_stripe_size;
            for(int i=0 ; i < 2 ; ++i ){ //顺序读写
                opt.rw_type = rw_seq[i];
                for(size_t j = 0 ; j < num_len ; ++j){//num
                    opt.thread_n = num[j];
                    for(size_t k = 0 ; k < qd_len ; ++k){ //qd
                        opt.iodepth = qd[k];
                        for(size_t m = 0 ; m < bs_seq_len ; ++m){ //bs
                            opt.block_size = bs_seq[m] * 1024;
                            if (m == 4) {
                                int is_duplicate = 0;
                                for (int x = 0; x < 4; ++x) {
                                    if (bs_seq[4] == bs_seq[x]) {
                                        is_duplicate = 1;
                                        break;
                                    }
                                }
                                if (is_duplicate) {
                                    printf("bs_seq[4] is duplicate, skip.\n");
                                    break;  // 或 return，如果你希望整个测试退出
                                }
                            }
                            if(opt.block_size)
                                libaio_run(&opt);
                        }
                    }
                }
            }
            plot(opt.fk_plot);
            goto done;
        case FAST_PLOT_IOPS://iops_log
            printf("处理 IOPS 绘图逻辑\n");
            //opt.size = 1024ULL * 1024 * 1024; //1G
            char *rw[] = {"randread", "randwrite"};
            int bs[] = {4, 16, 32};
            size_t bs_len = sizeof(bs) / sizeof(bs[0]);
            for(int i=0 ; i < 2 ; ++i ){ //随机读写
                opt.rw_type = rw[i];
                for(size_t j = 0 ; j < num_len ; ++j){//num
                    opt.thread_n = num[j];
                    for(size_t k = 0 ; k < qd_len ; ++k){ //qd
                        opt.iodepth = qd[k];
                        for(size_t m = 0 ; m < bs_len ; ++m){ //bs
                            opt.block_size = bs[m] * 1024;
                            if (m == 4) {
                                int is_duplicate = 0;
                                for (int x = 0; x < 4; ++x) {
                                    if (bs_seq[4] == bs_seq[x]) {
                                        is_duplicate = 1;
                                        break;
                                    }
                                }
                                if (is_duplicate) {
                                    printf("bs_seq[4] is duplicate, skip.\n");
                                    break;  // 或 return，如果你希望整个测试退出
                                }
                            }
                            if(libaio_run(&opt)){
                                printf("libaio read/write error...\n");
                                return 1;
                            }
                        }
                    }
                }
            }
            plot(opt.fk_plot);
            goto done;
        case FAST_PLOT_TAILLAT://lat_log
            // 执行尾延迟相关逻辑
            printf("处理尾延迟绘图逻辑\n");
            plot(opt.fk_plot);
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
    if(strcmp(opt.raid_cf.raid_status, "degrade") == 0){
        char cli[32];
        if (command_exists("storcli")) {
            printf("--- Found cli-cmd: storcli ---\n");
            strcpy(cli, "storcli");
        } else if (command_exists("storcli64")) {
            printf("---Found cli-cmd: storcli64---\n");
            strcpy(cli, "storcli64");
        } else {
            printf("--- Found cli-cmd: storcli ---\n");
            printf("---  未发现raid命令行工具...  ---\n");
            printf("---  请安装raid的管理工具...  ---\n");
            return 1;
        }
        set_raid_optl(cli, opt.raid_cf.raid_level);
    }
	return 0;
}