
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>

#include "raidio.h"

#include <ctype.h>

int get_ugood_disks(char *cli, pd_slot_t disks[], int max_disks) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),"%s /c0 show",cli);
    //printf("get_ugood_disks  show cmd: %s\n",cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[512];
    int in_pd_list = 0;
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        // 定位 PD LIST 区块
        if (strstr(line, "PD LIST")) {
            in_pd_list = 1;
            continue;
        }

        // 退出 PD LIST 区块
        if (in_pd_list && line[0] == '\n') {
            break;
        }

        // 跳过表头和分隔符行
        if (in_pd_list && (strstr(line, "EID:Slt") || strstr(line, "----"))) {
            continue;
        }

        // 处理有效行，查找 UGood
        if (in_pd_list && strstr(line, "UGood")) {
            int eid = -1, slot = -1;
            if (sscanf(line, "%d:%d", &eid, &slot) == 2) {
                if (count < max_disks) {
                    disks[count].eid = eid;
                    disks[count].slot = slot;
                    count++;
                }
            }
        }
    }

    pclose(fp);
    return count;
}

int create_raid(char *cli, const raid_config *conf) {
    pd_slot_t disks[64];
    int found = get_ugood_disks(cli, disks, 64);

    if (found < conf->num_members) {
        fprintf(stderr, "错误：可用磁盘不足，需 %d 个，实际仅 %d 个。\n",
                conf->num_members, found);
        return -1;
    }

    // 拼接 drives 参数
    char drives_part[512] = {0};
    for (int i = 0; i < conf->num_members; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d:%d", disks[i].eid, disks[i].slot);
        strcat(drives_part, buf);
        if (i < conf->num_members - 1)
            strcat(drives_part, ",");
    }

    // 构建命令
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s /c0 add vd size=all type=r%d drives=%s strip=%d",cli, 
        conf->raid_level, drives_part, conf->strip_size);

    printf("执行命令：%s\n", cmd);

    // 执行命令
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "RAID 创建失败，storcli 返回码: %d\n", ret);
        return -1;
    }

    printf("RAID 创建成功。\n");
    return 0;
}



int command_exists(const char *cmd) {
    char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char *path_copy = strdup(path_env);
    char *dir = strtok(path_copy, ":");
    while (dir) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        if (access(full_path, X_OK) == 0) {
            free(path_copy);
            return 1; // Command found
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);
    return 0; // Not found
}

uint64_t parse_size(const char *str) {
    int len = strlen(str);
    if (len < 2)
        return -1;

    char unit = toupper(str[len - 1]);
    long multiplier = 1;
    char number_part[32];

    strncpy(number_part, str, len - 1);
    number_part[len - 1] = '\0';

    switch (unit) {
        case 'K': multiplier = 1024; break;
        case 'M': multiplier = 1024 * 1024; break;
        case 'G': multiplier = 1024 * 1024 * 1024; break;
		case 'T': multiplier = 1024 * 1024 * 1024 * 1024; break;
        default:
            if (isdigit(unit)) {
                multiplier = 1;
                strncpy(number_part, str, len);
                number_part[len] = '\0';
            } else {
                fprintf(stderr, "Invalid size unit: %c\n", unit);
                return -1;
            }
    }

    int value = atoi(number_part);
    if (value <= 0) {
        fprintf(stderr, "Invalid numeric value: %s\n", number_part);
        return -1;
    }

    return value * multiplier;
}

void print_rio_args(const struct rio_args *args) {
    printf("===========================\n");
    printf("=== RIO Configuration ===\n");
    printf("File: %s\n", args->file);
    printf("Block size: %lu bytes\n", args->block_size);
    printf("RW Type: %s\n", args->rw_type);
    printf("IO Engine: %s\n", args->ioengine);
    printf("Size: %lu bytes\n", args->size);
    printf("IO Depth: %d\n", args->iodepth);
    printf("Threads: %d\n", args->thread_n);
    printf("Direct IO: %s\n", args->direct ? "Yes" : "No");

    // RAID Config
    printf("\n--- RAID Configuration ---\n");
    printf("RAID Type: %s\n", args->raid_cf.raid_type == RAID_TYPE_SOFT ? "Soft" : "Hard");
    printf("RAID Level: %d\n", args->raid_cf.raid_level);
    printf("Strip Size: %d KB\n", args->raid_cf.strip_size);
    printf("Write Cache: %s\n", args->raid_cf.wcache ? "Enabled" : "Disabled");
    printf("Read Cache: %s\n", args->raid_cf.rcache ? "Enabled" : "Disabled");
    printf("PD Cache: %s\n", args->raid_cf.pdcache ? "Enabled" : "Disabled");
    printf("Optimizer: %s\n", args->raid_cf.optimizer ? "Enabled" : "Disabled");
    printf("===========================\n");
}

void print_help(const char *progname) {
    printf("Usage: %s --file <path> [--bs <size>] [--rw <type>]\n", progname);
    printf("Options:\n");
    printf("  --file <path>   Required. Target file or device path\n");
    printf("  --bs <size>     Optional. Block size (default: 4096)\n");
    printf("  --rw <type>     Optional. Access type: read/write (default: read)\n");
	printf("  --ioengine <sync/async>     Optional. ioengine (default: libaio)\n");
	printf("  --size <io_size>     Optional. Request IO size\n");
	printf("  --iodepth <queue depth>     Optional. 并发IO数\n");
	printf("  --thread_n <multi threads>     Optional. 运行的线程数\n");
	printf("  --direct <page cache>     Optional. 是否绕过page cache\n");
	printf("  --wcache <wcache>     Optional. 硬件raid卡写回模式, 1为开启WB, 0为WT\n");
    printf("  --rcache <rcache>     Optional. 硬件raid卡read ahead模式, 1为开启RA, 0为noRA\n");
    printf("  --pdcache <pdcache>     Optional. 硬件raid卡设置pdcache模式, 1为开启, 0为关闭\n");
    printf("  --raid-type <soft/hard>     Optional. 软RAID or 硬件RAID, 对应参数值soft/hard\n");
    printf("  --raid-level <raid level>     Optional. RAID等级, 对应参数值soft/hard\n");
    printf("  --strip_size <strip size>     Optional. strip size,unit K; default 64KB\n");
    printf("  --help          Show this help message\n");
}


int rio_parse_options(int argc, char *argv[], struct rio_args *a)
{
	a->file = NULL;
	a->block_size = 4096;
	a->rw_type = "read";
	a->ioengine = "libaio";
	
	static struct option long_options[] = {     
		{"wcache", required_argument, 0, 'w'},
        {"raid_type", required_argument, 0, 'a'},
        {"bs", required_argument, 0, 'b'},
        {"rcache", required_argument, 0, 'c'},
        {"direct", required_argument, 0, 'd'},
        {"pdcache", required_argument, 0, 'e'},
        {"file", required_argument, 0, 'f'}, //filename
        {"raid_level", required_argument, 0, 'g'},
        {"ioengine", required_argument, 0, 'i'},
        {"strip_size", required_argument, 0, 'j'},
        {"iodepth", required_argument, 0, 'q'},
        {"rw", required_argument, 0, 'r'},
		{"size", required_argument, 0, 's'},
		{"thread_n", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    a->raid_cf.strip_size = 64 ;
    int opt;
    while ((opt = getopt_long(argc, argv, "f:b:r:h:s:i:q:t:d:w:a:c:e:g:j", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': a->file = optarg; break;
            case 'b':
				a->block_size = parse_size(optarg);
				if (a->block_size <= 0) {
					fprintf(stderr, "Invalid block size: %s\n", optarg);
					exit(EXIT_FAILURE);
				}else if(a->block_size > 128 *1024){
					a->thread_n = 1; a->iodepth = 16;
				} else{
					a->thread_n = 16; a->iodepth = 16;
				}
                break;
            case 'r': a->rw_type = optarg; break;
			case 's': a->size = parse_size(optarg);
				if (a->size <= 0) {
					fprintf(stderr, "Invalid block size: %s\n", optarg);
					exit(EXIT_FAILURE);
				}
                break;	
			case 'i': a->ioengine = optarg; break;
			case 'q': a->iodepth = atoi(optarg); break;
			case 't': a->thread_n = atoi(optarg); break;
			case 'd': a->direct = atoi(optarg); break;
			case 'w': a->raid_cf.wcache = atoi(optarg); break;
            case 'c': a->raid_cf.rcache = atoi(optarg); break;
            case 'a': 
                if (strcmp(optarg, "soft") == 0) {
                    a->raid_cf.raid_type = RAID_TYPE_SOFT;
                } else if (strcmp(optarg, "hard") == 0) {
                    a->raid_cf.raid_type = RAID_TYPE_HARD;
                } else if (strcmp(optarg, "none") == 0) {
                    a->raid_cf.raid_type = RAID_TYPE_NONE;
                } else {
                    fprintf(stderr, "Unknown RAID type: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'e': a->raid_cf.pdcache = atoi(optarg); break;
            case 'g': a->raid_cf.raid_level = atoi(optarg); break;
            case 'j': a->raid_cf.strip_size = atoi(optarg); break;
			case 'h': print_help(argv[0]); exit(0);
            default: print_help(argv[0]); return 1;
        }
    }
    print_rio_args(a);
    if (a->raid_cf.raid_type == RAID_TYPE_SOFT) { 
        // 软 RAID 分支逻辑
        printf("Detected soft RAID configuration.\n");
        return 0;
        // 在这里执行针对软 RAID 的处理
    } else if (a->raid_cf.raid_type == RAID_TYPE_HARD) {
        // 硬 RAID 分支逻辑
        printf("Detected hard RAID configuration.\n");
        char cli[10];
        if (command_exists("storcli")) {
            printf("Found: storcli\n");
            strcpy(cli, "storcli");
        } else if (command_exists("storcli64")) {
            printf("Found: storcli64\n");
            strcpy(cli, "storcli64");
        } else {
            printf("Neither storcli nor storcli64 found. If you are broadcom 96xx raid controller,Please choose new storcli tool\n");
        }
        //打印出raid系统的盘数
        // pd_slot_t ugood_disks[64];
        // int n = get_ugood_disks(ugood_disks, 64);
        // if (n <= 0) {
        //     printf("未找到任何 UGood 磁盘。\n");
        //     return 1;
        // }
        // printf("发现 %d 个 UGood 状态磁盘:\n", n);
        // for (int i = 0; i < n; ++i) {
        //     printf("  EID: %d, Slot: %d\n", ugood_disks[i].eid, ugood_disks[i].slot);
        // }
        create_raid(cli, &a->raid_cf);

        return 0;
        // 在这里执行针对硬 RAID 的处理
    } else if (a->raid_cf.raid_type == RAID_TYPE_NONE) {
        printf("Detected None RAID configuration.\n");
        return 0;
    } else {
        fprintf(stderr, "Unknown RAID type: %d\n", a->raid_cf.raid_type);
    }




    //return 1;
    if (!a->file) {
        fprintf(stderr, "Error: --file is required\n");
        print_help(argv[0]);
        return 1;
    }
	return 0;
}

int run_rio(struct rio_args *args){
    printf("TODO...   --call rio\n");
	printf("[RIO] file=%s, bs=%lld, rw=%s, ", args->file, args->block_size, args->rw_type);
	printf("ioengine=%s, size=%lld, iodepth=%d, thread_n=%d\n", args->ioengine, args->size, args->iodepth, args->thread_n);
	return 0;
}

void clean_rio(void)
{
	printf("TO DO ...   --free some resource!!!\n");
}

