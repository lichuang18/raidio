
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>

#include "raidio.h"

#include <ctype.h>

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
	printf("  --wrcache <wrcache>     Optional. 硬件raid卡写回模式, WT / WB\n");
    printf("  --help          Show this help message\n");
}


int rio_parse_options(int argc, char *argv[], struct rio_args *a)
{
	a->file = NULL;
	a->block_size = 4096;
	a->rw_type = "read";
	a->ioengine = "libaio";
	
	static struct option long_options[] = {
        {"file", required_argument, 0, 'f'}, //filename
        {"bs", required_argument, 0, 'b'},
        {"rw", required_argument, 0, 'r'},
		{"ioengine", required_argument, 0, 'i'},
		{"size", required_argument, 0, 's'},
		{"iodepth", required_argument, 0, 'q'},
		{"thread_n", required_argument, 0, 't'},
		{"direct", required_argument, 0, 'd'},	
		{"wrcache", required_argument, 0, 'w'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:b:r:h:s:i:q:t:d:w", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                a->file = optarg;
                break;
            case 'b':
				a->block_size = parse_size(optarg);
				if (a->block_size <= 0) {
					fprintf(stderr, "Invalid block size: %s\n", optarg);
					exit(EXIT_FAILURE);
				}else if(a->block_size > 128 *1024){
					a->thread_n = 1;
					a->iodepth = 16;
				} else{
					a->thread_n = 16;
					a->iodepth = 16;
				}
                break;
            case 'r':
                a->rw_type = optarg;
                break;
			case 's':
				a->size = parse_size(optarg);
				if (a->size <= 0) {
					fprintf(stderr, "Invalid block size: %s\n", optarg);
					exit(EXIT_FAILURE);
				}
                break;	
			case 'i':
                a->ioengine = optarg;
                break;
			case 'q':
                a->iodepth = atoi(optarg);
                break;
			case 't':
                a->thread_n = atoi(optarg);
                break;
			case 'd':
                a->direct = atoi(optarg);
                break;
			case 'w':
                a->wrcache = optarg;
                break;
			case 'h':
                print_help(argv[0]);
                exit(0);
            default:
                print_help(argv[0]);
                return 1;
        }
    }
	
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

