#include "raidio.h"
#include <stdio.h>


// ./rio -f sda -b 128K -ioengine libaio -q 6 --size 100M --thread_n 2 --rw=write

int main(int argc, char *argv[]) //todo, add char *envp[]
{
	int ret = 1;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--file=", 7) == 0) {
            file = argv[i] + 7; 
            break;
        }
    }
    struct rio_args opt;
	if (rio_parse_options(argc, argv, &opt))
		goto done;
    
	if (!is_raid(opt.file)) {
        if (strcmp(opt.rw_type, "read") != 0 &&
            strcmp(opt.rw_type, "randread") != 0 &&
            strcmp(opt.rw_type, "write") != 0 &&
            strcmp(opt.rw_type, "randwrite") != 0) {
            fprintf(stderr, "Error: Unsupported rw_type: '%s'. Must be one of: read, randread, write, randwrite.\n", opt.rw_type);
            exit(EXIT_FAILURE);
        } else{
            printf("[1] Start %s task call...\n",opt.rw_type);
            ret = libaio_run(&opt);
        }
	} else
		ret = run_rio(&opt);

done:
	clean_rio();
	return ret;
}