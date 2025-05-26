
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>

#include "raidio.h"

#include <ctype.h>

#define MAX_LINE 512

int check_vd_initialization_success(const char *cli, int vd_number) {

    if (!cli) {
        fprintf(stderr, "cli is NULL!\n");
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 show events > /tmp/raid_events", cli);
    char check_des[256];
    snprintf(check_des, sizeof(check_des), "Initialization complete on VD %02d", vd_number);
   
    int ret = system(cmd);

    FILE *fp = fopen("/tmp/raid_events", "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    // FILE *fp = popen(cmd, "r");
    // if (!fp) {
    //     perror("popen");
    //     return -1;
    // }

    char lines[1024][512];  // 最多缓存1024行，每行最多512字节

    int head = 0, count = 0;

    while (fgets(lines[head], sizeof(lines[0]), fp)) {
        lines[head][strcspn(lines[head], "\n")] = '\0';
        head = (head + 1) % 1024;
        if (count < 1024) count++;
    }
    // 倒着找最近 8 个 Event Description
    int found = 0;
    for (int i = 0; i < count && found < 8; i++) {
        int idx = (head - 1 - i + 1024) % 1024;
        char *line = lines[idx];
        while (*line == ' ' || *line == '\t') line++;
        //printf("[Event  ] %s\n", line);
        if (strncmp(line, "Event Description:",18) == 0) {
            found++;
            printf("[Event %d] %s\n", found, line);
            if (strstr(line, check_des)) {
                //free(lines);
                return 0;  // 找到成功事件
            }
        }
    }
    // 未找到初始化完成事件
    //free(lines);
    return -1;
}

int get_vd_id_by_name(const char *cli, const char *target_name) {

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 /vall show", cli);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[512];
    int found_vd_section = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Virtual Drives :")) {
            found_vd_section = 1;
            continue;
        }

        if (found_vd_section && strstr(line, target_name)) {
            // 解析 VD 字段（例如 "0/2"），提取后面的数字
            //printf("get vd_num | %s\n",line);
            int dg, vd;
            if (sscanf(line, "%d/%d", &dg, &vd) == 2) {
                pclose(fp);
                return vd;  // 返回 VD 编号（后面这个数字）
            }
        }
    }

    pclose(fp);
    return -1;  // 未找到
}

int full_init_raid(const char *cli, int vd_number) {
    // 构建初始化命令
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 /v%d start init full", cli, vd_number);


    printf("执行命令：%s\n", cmd);

    // 执行命令
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "执行初始化命令失败，ret=%d\n", ret);
        return -1;
    }

    while (1) {
        int check_state = check_vd_initialization_success(cli, vd_number);
        if (check_state == 0) {
            printf("VD %02d 初始化完成！\n", vd_number);
            break;
        }
        printf("VD %02d 正在初始化中，30 秒后重新检查...\n", vd_number);
        sleep(30);
    }

    printf("RAID 初始化成功。\n");
    return 0;
}


// 提取最近新增的 /dev/sdX 盘符（在 create_raid 调用之后出现）
int find_new_disk_after_time(time_t start_time, char *disk_name, size_t len) {
    FILE *fp = popen("dmesg --ctime", "r");
    if (!fp) {
        perror("popen dmesg");
        return -1;
    }

    char line[MAX_LINE];
    char latest_disk[32] = "";
    time_t latest_ts = 0;

    while (fgets(line, sizeof(line), fp)) {
        // 只处理包含 "Attached SCSI disk" 的行
        if (strstr(line, "Attached SCSI disk")) {
            // 解析时间字符串
            char time_str[64];
            if (sscanf(line, "[%63[^]]]", time_str) == 1) {
                struct tm t = {0};
                if (strptime(time_str, "%a %b %d %T %Y", &t)) {
                    time_t log_time = mktime(&t);
                    if (difftime(log_time, start_time) >= 0 && log_time > latest_ts) {
                        // 查找第二个中括号内的盘符，如 "[sdb]"
                        int count = 0;
                        for (int i = 0; i < MAX_LINE && line[i]; i++) {
                            if (line[i] == '[') {
                                count++;
                                if (count == 2) {
                                    int start = i + 1;
                                    int end = start;
                                    int j = 0;
                                    while (line[end] && line[end] != ']' && j < 31) {
                                        latest_disk[j++] = line[end++];
                                    }
                                    latest_disk[j] = '\0';  // 添加字符串结尾
                                    break;
                                }
                        
                            }
                        }
                    }
                }
            }
        }
    }

    pclose(fp);
    //printf("%s\n",latest_disk); //sda
    if (strlen(latest_disk) > 0) {
        snprintf(disk_name, len, "/dev/%s", latest_disk);
        return 0;
    } else {
        return -1;
    }
}


// 判断某一行是否匹配目标 filename，并且其第3列为 BROADCOM 或 LSI
int check_lsscsi_vendor(const char *filename) {
    FILE *fp = popen("lsscsi", "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // 查找包含目标设备的行（例如 /dev/sda）
        if (strstr(line, filename)) {
            // 拷贝字符串进行字段分割
            char tmp[512];
            strncpy(tmp, line, sizeof(tmp));
            tmp[sizeof(tmp) - 1] = '\0';

            // 按空格分隔字段
            char *token = strtok(tmp, " \t\n");
            int col = 0;
            while (token != NULL) {
                col++;
                if (col == 3) {
                    // 判断厂商是否为 BROADCOM 或 LSI
                    if (strcmp(token, "BROADCOM") == 0 || strcmp(token, "LSI") == 0 || strcmp(token, "AVAGO") == 0) {
                        pclose(fp);
                        return 1;  // 匹配成功
                    } else {
                        pclose(fp);
                        return 0;  // 第3列不是目标厂商
                    }
                }
                token = strtok(NULL, " \t\n");
            }
        }
    }

    pclose(fp);
    return 0;  // 没找到对应 filename 行或没有匹配
}


int get_ugood_disks(char *cli, pd_slot_t disks[], int max_disks) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 show", cli);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[512];
    int in_pd_list = 0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        // 进入 PD LIST 区域
        if (!in_pd_list && strstr(line, "PD LIST")) {
            in_pd_list = 1;
            continue;
        }

        // 退出 PD LIST 区域
        if (in_pd_list && strstr(line, "Enclosure LIST")) {
            break;
        }

        // 跳过表头和分隔符行
        if (in_pd_list && (strstr(line, "EID:Slt") || strstr(line, "----"))) {
            continue;
        }

        // 处理含有 UGood 的行
        if (in_pd_list && strstr(line, "UGood")) {
            char eid_slt[16];
            if (sscanf(line, "%15s", eid_slt) == 1) {
                int eid, slot;
                if (sscanf(eid_slt, "%d:%d", &eid, &slot) == 2) {
                    if (count < max_disks) {
                        disks[count].eid = eid;
                        disks[count].slot = slot;
                        count++;
                    }
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
        "%s /c0 add vd r%d size=%dGB name=%s drives=%s strip=%d",cli, 
        conf->raid_level, conf->capabilty, conf->raid_name, drives_part, conf->strip_size);

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
    printf("Num_members: %d Disks\n", args->raid_cf.num_members);
    printf("Capability: %d GB\n", args->raid_cf.capabilty);
    printf("Raid_name: %s \n", args->raid_cf.raid_name);
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
	printf("  --direct <page cache>     Optional. 是否绕过page cache,default\n");
	printf("  --wcache <wcache>     Optional. 硬件raid卡写回模式, 1为开启WB, 0为WT,default 0\n");
    printf("  --rcache <rcache>     Optional. 硬件raid卡read ahead模式, 1为开启RA, 0为noRA,default 0\n");
    printf("  --pdcache <pdcache>     Optional. 硬件raid卡设置pdcache模式, 1为开启, 0为关闭,default 0\n");
    printf("  --raid-type <soft/hard>     Optional. 软RAID or 硬件RAID, 对应参数值soft/hard,default hard\n");
    printf("  --raid-level <raid level>     Optional. RAID等级, 对应参数值soft/hard,default 0\n");
    printf("  --strip_size <strip size>     Optional. strip size,unit K; default 64KB\n");
    printf("  --num_memebers <disks>     Optional. 硬件raid卡组建的raid成员数,default 1\n");
    printf("  --capability <disks size>     Optional. 硬件raid卡组建的raid盘容量,default 100GB\n");
    printf("  --raid_name <raid name>     Optional. 硬件raid卡组建的raid名称\n");
    printf("  --help          Show this help message\n");
}


int rio_parse_options(int argc, char *argv[], struct rio_args *a)
{
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
        {"num_members", required_argument, 0, 'k'},
        {"capability", required_argument, 0, 'l'},
        {"raid_name", required_argument, 0, 'm'},
        {"iodepth", required_argument, 0, 'q'},
        {"rw", required_argument, 0, 'r'},
		{"size", required_argument, 0, 's'},
		{"thread_n", required_argument, 0, 't'}, 
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    //defaults
    a->file[0] = '\0';
	a->block_size = 4096;
	a->rw_type = "read";
	a->ioengine = "libaio";
    a->direct = 1 ;
    //defaults raid config
    a->raid_cf.strip_size = 64 ; //KB
    a->raid_cf.num_members = 1 ;
    a->raid_cf.capabilty = 100 ; //GB
    a->raid_cf.raid_type = RAID_TYPE_HARD ; //GB
    a->raid_cf.raid_level = 0 ; //GB
    a->raid_cf.raid_name[0] = '\0'; 

    int opt;
    while ((opt = getopt_long(argc, argv, "f:b:r:h:s:i:q:t:d:w:a:c:e:g:j:k:l:m", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': 
                strncpy(a->file, optarg, sizeof(a->file) - 1);
                a->file[sizeof(a->file) - 1] = '\0';  // 确保结尾安全
                break;
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
            case 'k': a->raid_cf.num_members = atoi(optarg); break;
            case 'm': 
                strncpy(a->raid_cf.raid_name, optarg, sizeof(a->raid_cf.raid_name) - 1);
                a->raid_cf.raid_name[sizeof(a->raid_cf.raid_name) - 1] = '\0';  // 确保结尾
                break;
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
        if(a->file[0] ==  '\0'){
            printf("Device is NULL, Prepare create RAID...\n");
            
            strncpy(a->file, "/dev/sda", sizeof(a->file) - 1);
            a->file[sizeof(a->file) - 1] = '\0'; 
            return 0; //注意删除，调试用

            char cli[32];
            if (command_exists("storcli")) {
                printf("Found cli cmd: storcli\n");
                strcpy(cli, "storcli");
            } else if (command_exists("storcli64")) {
                printf("Found cli cmd: storcli64\n");
                strcpy(cli, "storcli64");
            } else {
                printf("Neither storcli nor storcli64 found. If you are broadcom 96xx raid controller,Please choose new storcli tool\n");
            }
            printf("============================================\n");
            printf("=== Start Create RAID with Configuration ===\n");
            printf("===    会按序号依次选取指定数量,组建raid      ===\n");
            printf("===    如需指定slt号组建raid,请先手动创建     ===\n");

            time_t start = time(NULL);
            if (create_raid(cli, &a->raid_cf) == 0) {
                sleep(5);  // 可选：给内核一点时间完成注册
                if (find_new_disk_after_time(start, a->file, 11) == 0) {
                    printf("New RAID device appeared: %s\n", a->file);
                } else {
                    printf("Failed to detect new RAID disk in dmesg.\n");
                    return 1;
                }
            } else{
                printf("RAID Create failed...\n");
                return 1;
            }
            int vd_number = get_vd_id_by_name(cli, a->raid_cf.raid_name); 
            if(vd_number < 0){
                printf("get RAID VD number failed...\n");
                return 1;
            }
            printf("vd_number: %d\n",vd_number);
            printf("===          Start Full Init RAID        ===\n");
            printf("cli: %s\n",cli);
            if(full_init_raid(cli, vd_number)){
                printf("Full Init RAID failed...\n");
                return 1;
            }
            //if(创建失败 或者 init 失败)  退出测试
            printf("===           End Full Init RAID         ===\n");
            printf("Add VD File: %s\n", a->file);
            return 0;
        } else {
            int result = check_lsscsi_vendor(a->file);
            if (result == 1) {
                printf("Device %s vendor is BROADCOM or LSI, so it is RAID\n", a->file);
            } else if (result == 0) {
                printf("Device %s vendor is not BROADCOM/LSI or not found\n", a->file);
                printf("Device %s maybe is nonRAID, go on normal disk test...\n", a->file);
                return 0;
            } else {
                printf("Error running lsscsi\n");
            }
        }
        // 在这里执行针对硬 RAID 的处理
    } else if (a->raid_cf.raid_type == RAID_TYPE_NONE) {
        printf("Detected None RAID configuration.\n");
        return 0;
    } else {
        fprintf(stderr, "Unknown RAID type: %d\n", a->raid_cf.raid_type);
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

