
#define _XOPEN_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>

#include "raidio.h"

#include <ctype.h>
pd_info_t g_pd_list[MAX_PDS]={0};

#define MAX_LINE 512
#define MAX_VDS 512

void set_raid_optl(const char *cli, int raid_level) {
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 /e%d/s%d set online", cli, g_pd_list[0].eid, g_pd_list[0].slt);
    system(cmd);
    if(raid_level == 6){
        snprintf(cmd, sizeof(cmd), "%s /c0 /e%d/s%d set online", cli, g_pd_list[1].eid, g_pd_list[1].slt);
        system(cmd);
    }
    //todo  检查raid配置onln是否生效的逻辑
}

int get_all_vd_ids(const char *cli, int *vd_ids, int max_vds) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 /vall show", cli);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[512];
    int found_vd_section = 0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (!found_vd_section) {
            if (strstr(line, "Virtual Drives :")) {
                found_vd_section = 1;
            }
            continue;
        }
        // 到了 VD 信息区域，尝试提取格式为 x/y 的 VD 编号
        int dg, vd;
        if (sscanf(line, "%d/%d", &dg, &vd) == 2) {
            if (count < max_vds) {
                vd_ids[count++] = vd;
            } else {
                fprintf(stderr, "超过最大 VD 数量限制 %d\n", max_vds);
                break;
            }
        }
    }

    pclose(fp);
    return count;  // 返回解析到的 VD 数量
}

int get_vd_number_from_sd(const char *cli, char *disk_name){
    int vd_ids[MAX_VDS];
    int count = get_all_vd_ids(cli, vd_ids, MAX_VDS);
    if (count < 0) {
        fprintf(stderr, "获取 VD 列表失败\n");
        return -1;
    }
    int ret = -1;
    printf("get_vd:  本台机器一共 %d 个 VD...\n", count);
    for (int i = 0; i < count; ++i) {
        
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d show all | awk -F'= ' '/OS Drive Name/ {print $2}'", cli, vd_ids[i]);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            perror("popen");
            return -1; // 错误
        }

        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            // 去掉末尾换行符（fgets 会保留 '\n'）
            buf[strcspn(buf, "\n")] = '\0';

            if (strcmp(buf, disk_name) == 0) {
                pclose(fp);
                ret = vd_ids[i];
                goto FOUNDER;
            } else {
                printf("get_vd:  查询vd操作失败, 未找到对应vd...\n");
                pclose(fp);
                return -1;
            }
        } else {
            printf("get_vd:  查询vd操作失败, 读取失败...\n");
            pclose(fp);
            return -1;
        }
        pclose(fp);
    }
FOUNDER:    
    return ret;
}


int check_vd_initialization_success(const char *cli, int vd_number) {

    if (!cli) {
        fprintf(stderr, "cli is NULL!\n");
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s /c0 show events > /tmp/raid_events", cli);
    char check_des[256];
    // snprintf(check_des, sizeof(check_des), "Initialization complete on VD %02d", vd_number);
    snprintf(check_des, sizeof(check_des), "Initialization complete on VD %02x", vd_number);
    system(cmd);

    FILE *fp = fopen("/tmp/raid_events", "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }
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
    //free(lines);
    return -1;
}



void set_raid_degrade(const char *cli, int raid_level, int vd_number) {
    
    if(raid_level < 5){
        printf("--- 当前RAID不支持degrade ---");
        return ;
    }

    char show_cmd[256];
    char cmd[256];
    snprintf(show_cmd, sizeof(show_cmd), "%s /c0 /v%d show all", cli, vd_number);
    
    FILE *fp = popen(show_cmd, "r");
    if (!fp) {
        perror("popen");
        return;
    }
    char line[1024];
    int in_target_section = 0;
    int onln_count = 0;
    int target_onln_count = raid_level - 4; // 按照 vd_number 控制需要找几行 Onln

    char vd_tag[64];
    snprintf(vd_tag, sizeof(vd_tag), "PDs for VD %d", vd_number);

    int separator_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        // 打印每行内容调试
        // 第一步：定位段落
        if (!in_target_section) {
            if (strstr(line, vd_tag)) {
                in_target_section = 1;
                if(DEBUG) printf(">>> 进入目标段落\n");
            }
            continue;
        }
        

        // 第二步：数分隔线，直到第2条后才开始处理数据
        if (strstr(line, "---------------") != NULL) {
            separator_count++;
            continue;
        }

        // 第三步：如果遇到解释说明段，退出
        if (strstr(line, "EID=")) {
            printf(">>> 结束于说明段\n");
            break;
        }
        if(DEBUG) printf("DEBUG: %s", line);
        // 第四步：开始处理数据行（在第二条分隔线之后）
        if (separator_count >= 2 && strstr(line, " Onln")) {
            if(DEBUG) printf(">>> 匹配 Onln 行: %s", line);
            int eid, slt;
            if (sscanf(line, "%d:%d", &eid, &slt) == 2) {
                printf("EID:%d Slt:%d\n", eid, slt);
                g_pd_list[onln_count].eid = eid;
                g_pd_list[onln_count].slt = slt;

                snprintf(cmd, sizeof(cmd), "%s /c0 /e%d/s%d set offline", cli, eid, slt);
                if(DEBUG) printf("执行命令: %s\n", cmd);
                system(cmd);

                onln_count++;
                if (onln_count >= target_onln_count)
                    break;
            } else {
                printf(">>> WARNING: 行解析失败: %s", line);
            }
        }
    }

    pclose(fp);
    //todo  检查raid配置degrade是否生效的逻辑
}


void set_raid(const char *cli, int wcache, int rcache, int pdcache,  int vd_number) {
    printf("执行set raid逻辑...配置raid盘(%d)参数,如, 用(%s)设置\n", vd_number, cli);
    char cmd[256];
    if(wcache){
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set wrcache=AWB", cli, vd_number);
    } else{
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set wrcache=WT", cli, vd_number);
    }
    system(cmd);

    if(rcache){
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set rdcache=ra", cli, vd_number);
    } else{
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set rdcache=nora", cli, vd_number);
    }
    system(cmd);

    if(pdcache){
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set pdcache=on", cli, vd_number);
    } else{
        snprintf(cmd, sizeof(cmd), "%s /c0 /v%d set pdcache=off", cli, vd_number);
    }
    system(cmd);
    //todo  检查raid配置cache是否生效的逻辑
}

void optimizer_raid(u_int64_t block_size, const char *cli, char *disk_name) {
    printf("执行优化raid逻辑...基于不同bs(%" PRIu64 ")配置raid盘(%s)内核参数, 或用(%s)优化\n", block_size, disk_name, cli);
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


    //printf("执行命令：%s\n", cmd);

    // 执行命令
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "init_raid:  执行初始化命令失败，ret=%d\n", ret);
        return -1;
    }

    while (1) {
        int check_state = check_vd_initialization_success(cli, vd_number);
        if (check_state == 0) {
            printf("init_raid:  VD %02d 初始化完成！\n", vd_number);
            break;
        }
        printf("init_raid:  VD %02d 正在初始化中，30 秒后重新检查...\n", vd_number);
        sleep(30);
    }

    printf("init_raid:  RAID 初始化成功。\n");
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
        conf->raid_level, conf->capability, conf->raid_name, drives_part, conf->strip_size);

    //printf("执行命令：%s\n", cmd);

    // 执行命令
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "RAID 创建失败，storcli 返回码: %d\n", ret);
        return -1;
    }

    //printf("RAID 创建成功。\n");
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
            return 1;
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);
    return 0;
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
        case 'G': multiplier = 1024ULL * 1024 * 1024; break;
		case 'T': multiplier = 1024ULL * 1024 * 1024 * 1024; break;
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
    printf("==========Test Conf========\n");
    printf("\n=== RIO Configuration ===\n");
    printf("File:         %s\n", args->file);
    printf("Block size:   %lu bytes\n", args->block_size);
    printf("RW Type:      %s\n", args->rw_type);
    printf("IO Engine:    %s\n", args->ioengine);
    printf("Size:         %lu bytes\n", args->size);
    printf("IO Depth:     %d\n", args->iodepth);
    printf("Threads:      %d\n", args->thread_n);
    printf("Direct IO:    %s\n", args->direct ? "Yes" : "No");

    if(args->raid_cf.raid_type == RAID_TYPE_SOFT || args->raid_cf.raid_type == RAID_TYPE_HARD){
        // RAID Config
        printf("\n--- RAID Conf(default) ---\n");
        if (args->raid_cf.raid_type == RAID_TYPE_SOFT) { 
            printf("RAID Type:    soft RAID\n");
        } else if (args->raid_cf.raid_type == RAID_TYPE_HARD) {
            printf("RAID Type:    hard RAID\n");
        } else{
            printf("RAID Type:    none RAID\n");
        }
        printf("RAID Level:   %d\n", args->raid_cf.raid_level);
        printf("Strip Size:   %d KB\n", args->raid_cf.strip_size);
        printf("Num_members:  %d Disks\n", args->raid_cf.num_members);
        printf("Capability:   %d GB\n", args->raid_cf.capability);
        printf("Raid_name:    %s \n", args->raid_cf.raid_name);
        printf("Write Cache:  %s\n", args->raid_cf.wcache ? "Enabled" : "Disabled");
        printf("Read Cache:   %s\n", args->raid_cf.rcache ? "Enabled" : "Disabled");
        printf("PD Cache:     %s\n", args->raid_cf.pdcache ? "Enabled" : "Disabled");
        printf("Optimizer:    %s\n", args->raid_cf.optimizer ? "Enabled" : "Disabled");
    }
    // fk_plot Config
    printf("\n--- plot Configuration ---\n");
    if (args->fk_plot == FAST_PLOT_BW) { 
        printf("Plot Type:    bw\n");
    } else if (args->fk_plot== FAST_PLOT_IOPS) {
        printf("Plot Type:    iops\n");
    } else if (args->fk_plot== FAST_PLOT_TAILLAT) {
        printf("Plot Type:    tail-latency\n");
    } else{
        printf("Plot Type:    none\n");
    }
    printf("===========================\n");
}

void print_help(const char *progname) {
    printf("Usage: %s --raid_type <none|hard|soft> --file <path> [--bs <size>] [--rw <type>]...\n", progname);
    printf("Example 1      JBOD: %s --raid_type none --file /dev/sda --bs 256K --rw write --size 1G --iodepth 16 --thread_n 4\n", progname);
    printf("Example 2 Hard RAID: %s --raid_type hard --file /dev/sda --bs 1024K --rw read --size 1G --iodepth 16 --thread_n 4\n", progname);
    printf("Example 3 Soft RAID: %s --raid_type soft --file /dev/sda --bs 16K --rw write --size 1G --iodepth 16 --thread_n 4\n", progname);
    printf("Example 4   fk_plot: %s --raid_type hard --file /dev/sda  --size 1G --fk_plot bw\n", progname);
    printf("Options:\n");
    printf("===== normal参数 =====\n");
    printf("  --file <path>                 Optional. 测试对象, 盘符或者文件，如果不指定，可能会触发raid的创建\n");
    printf("  --bs <size>                   Optional. Block size, default 4096)\n");
    printf("  --rw <type>                   Optional. Access type: read/write, default read)\n");
	printf("  --ioengine <sync/async>       Optional. ioengine, default libaio)\n");
	printf("  --size <io_size>              Optional. 下发IO请求的size, default 100M\n");
	printf("  --iodepth <queue depth>       Optional. 并发IO数, default 16\n");
	printf("  --thread_n <multi threads>    Optional. 运行的线程数, default 1\n");
	printf("  --direct <page cache>         Optional. 是否绕过page cache, default 1\n");
    printf("===== raid参数 =====\n");
	printf("  --wcache <wcache>             Optional. 硬件raid卡写回模式, 1为开启WB, 0为WT, default 0\n");
    printf("  --rcache <rcache>             Optional. 硬件raid卡read ahead模式, 1为开启RA, 0为noRA, default 0\n");
    printf("  --pdcache <pdcache>           Optional. 硬件raid卡设置pdcache模式, 1为开启, 0为关闭, default 0\n");
    printf("  --raid-type <soft/hard>       Required. 软RAID or 硬件RAID, 对应参数值soft/hard, default hard\n");
    printf("  --raid-level <raid level>     Optional. RAID等级, 对应参数值soft/hard, default 0\n");
    printf("  --strip_size <strip size>     Optional. strip size,unit K, default 64KB\n");
    printf("  --num_memebers <disks>        Optional. 硬件raid卡组建的raid成员数, default 1\n");
    printf("  --capability <disks size>     Optional. 硬件raid卡组建的raid盘容量, default 100GB\n");
    printf("  --raid_name <raid name>       Optional. 硬件raid卡组建的raid名称\n");
    printf("===== 快速测试 =====\n");
    printf("  --fk_plot <plot>              Optional. raid测试快速可视化结果\n");
    // printf("  --fk_size <test size>     Optional. raid测试可视化的下发IOsize大小, defaults(1G)\n");
    printf("  --help                        Show this help message\n");
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
        {"capability", required_argument, 0, 'l'}, //capability 
        {"raid_name", required_argument, 0, 'm'},
        {"fk_plot", required_argument, 0, 'n'},
        {"optimizer", required_argument, 0, 'o'},
        {"raid_status", required_argument, 0, 'p'},
        {"iodepth", required_argument, 0, 'q'},
        {"rw", required_argument, 0, 'r'},
		{"size", required_argument, 0, 's'},
		{"thread_n", required_argument, 0, 't'}, 
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    //defaults
    a->file[0] = '\0'; //  /dev/sda
	a->block_size = 1048576;//   seq bs默认 1M
	a->rw_type = "read";
	a->ioengine = "libaio";
    a->size = 100ULL * 1024 * 1024;
    a->iodepth = 16;
    a->thread_n = 1;
    a->direct = 1 ;
    //defaults    raid_config raid_cf
    a->raid_cf.raid_type = RAID_TYPE_HARD;
    a->raid_cf.raid_level = 0 ; 
    a->raid_cf.strip_size = 64 ;
    a->raid_cf.num_members = 1 ;
    a->raid_cf.wcache = 0;
    a->raid_cf.rcache = 0;
    a->raid_cf.pdcache = 0;
    a->raid_cf.optimizer = 0; //默认不优化
    a->raid_cf.capability = 100 ; //GB 
    a->raid_cf.raid_name[0] = '\0'; 
    strcpy(a->raid_cf.raid_status, "optl"); 
    //defaults  fast_plots fk_plot;
    a->fk_plot = FAST_PLOT_NONE;
    // a->fk_size = 1024ULL * 1024 * 1024;

    int opt;
    while ((opt = getopt_long(argc, argv, "f:r:b:h:s:i:q:t:d:w:a:c:e:g:j:k:l:m:n:o:p", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': 
                strncpy(a->file, optarg, sizeof(a->file) - 1);
                a->file[sizeof(a->file) - 1] = '\0';  // 确保结尾安全
                break;
            case 'r': a->rw_type = optarg; 
                if (strcmp(a->rw_type, "randread") == 0 || strcmp(a->rw_type, "randwrite") == 0) {
                    a->block_size = 4096;   // rand默认bs 4K
                }
                break;
            case 'b':
				a->block_size = parse_size(optarg);
				if (a->block_size <= 0) {
					fprintf(stderr, "Invalid block size: %s\n", optarg);
					exit(EXIT_FAILURE);
				}else if(a->block_size > 128 *1024){
					a->thread_n = 1; 
				} else{
					a->thread_n = 16;
				}
                break;
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
            case 'l': a->raid_cf.capability = atoi(optarg); break;
            case 'm': 
                strncpy(a->raid_cf.raid_name, optarg, sizeof(a->raid_cf.raid_name) - 1);
                a->raid_cf.raid_name[sizeof(a->raid_cf.raid_name) - 1] = '\0';  // 确保结尾
                break;
            case 'n': 
                if (strcmp(optarg, "bw") == 0) {
                    a->fk_plot = FAST_PLOT_BW;
                } else if (strcmp(optarg, "iops") == 0) {
                    a->fk_plot = FAST_PLOT_IOPS;
                } else if (strcmp(optarg, "lat") == 0) {
                    a->fk_plot = FAST_PLOT_TAILLAT;
                } else {
                    fprintf(stderr, "Unknown fk_plot type: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'o': a->raid_cf.optimizer = atoi(optarg); break;
            case 'p': 
                strncpy(a->raid_cf.raid_status, optarg, sizeof(a->raid_cf.raid_status) - 1);
                a->raid_cf.raid_status[sizeof(a->raid_cf.raid_status) - 1] = '\0';  // 确保结尾安全
                break;    
			case 'h': print_help(argv[0]); exit(0);
            default: print_help(argv[0]); return 1;
        }
    }

    print_rio_args(a);
    if (a->raid_cf.raid_type == RAID_TYPE_SOFT) { 
        printf("\n=========hard raid=========\n");
        printf("Detected soft RAID...\n");
        printf("\n=========  TO DO  =========\n");
        return 0;
    } else if (a->raid_cf.raid_type == RAID_TYPE_HARD) {
        printf("\n=========hard raid=========\n");
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
        if(a->file[0] ==  '\0'){
            printf("---    Device is NULL   ---\n");
            printf("---  Start Create RAID  ---\n");
            printf("--- 会按照顺序选择slt创建  ---\n");
            printf("--- 如果需要指定slt创建，  ---\n");
            printf("---    请提前创建RAID！   ---\n");
            
            time_t start = time(NULL);
            if (create_raid(cli, &a->raid_cf) == 0) {
                sleep(5);  // 可选：给内核一点时间完成注册
                if (find_new_disk_after_time(start, a->file, 11) == 0) {
                    printf("create_raid:  New RAID device appeared [%s]...\n", a->file);
                } else {
                    printf("create_raid:  Failed to detect new RAID disk in dmesg...\n");
                    return 1;
                }
            } else{
                printf("create_raid:  RAID Create failed...\n");
                return 1;
            }
            int vd_number = get_vd_id_by_name(cli, a->raid_cf.raid_name); 
            if(vd_number < 0){
                printf("create_raid:  get RAID VD number failed...\n");
                return 1;
            }
            printf("create_raid:  vd_number [%d]\n",vd_number);
            printf("--- Create RAID Success ---\n");

            printf("\n---    Full Init RAID   ---\n");
            if(DEBUG) printf("cli: %s\n",cli);
            if(full_init_raid(cli, vd_number)){
                printf("init_raid:  Full Init RAID failed...\n");
                return 1;
            }
            printf("---  init RAID success  ---\n");
            printf("Add VD%d -> File: %s\n", vd_number, a->file);
            set_raid(cli, a->raid_cf.wcache, a->raid_cf.rcache, a->raid_cf.pdcache, vd_number);//todo
            if(strcmp(a->raid_cf.raid_status, "degrade") == 0){
                set_raid_degrade(cli, a->raid_cf.raid_level, vd_number);
            }
            if(a->raid_cf.optimizer){
                printf("\n--- Try Optimizer RAID  ---\n");
                optimizer_raid(a->block_size, cli, a->file);//todo
            }
            printf("===========================\n");
            return 0;
        } else {
            int result = check_lsscsi_vendor(a->file);
            printf("---     Device exist    ---\n");
            if (result == 1) {
                printf("--- %s vendor: BROADCOM ---\n", a->file);
                int vd_number = get_vd_number_from_sd(cli,a->file);
                set_raid(cli, a->raid_cf.wcache, a->raid_cf.rcache, a->raid_cf.pdcache, vd_number);
                if(strcmp(a->raid_cf.raid_status, "degrade") == 0){
                    set_raid_degrade(cli, a->raid_cf.raid_level, vd_number);
                }
                if(a->raid_cf.optimizer){
                    printf("\n--- Try Optimizer RAID  ---\n");
                    optimizer_raid(a->block_size, cli, a->file);
                }

            } else if (result == 0) {
                printf("--- %s vendor: not BROADCOM ---\n", a->file);
                printf("---  Device maybe not RAID  ---\n");
                printf("---     Go on JBOD test     ---\n");
                goto none_raid;
            } else {
                printf("Error running lsscsi\n");
            }
        }
    } else if (a->raid_cf.raid_type == RAID_TYPE_NONE) {
none_raid:
        printf("\n=========none raid=========\n");
        return 0;
    } else {
        fprintf(stderr, "Unknown RAID type: %d\n", a->raid_cf.raid_type);
    }
    
	return 0;
}

void plot(fast_plots fk_plot){

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "python3 src/plot.py result/raid-%s-results.log %s", fast_plot_names[fk_plot], fast_plot_names[fk_plot]);
    int ret = system(cmd);
    if (ret != 0) {
        printf("plot failed, [error: %d]...\n", ret);
    }
    printf("Generate plot successful...\n");
}


