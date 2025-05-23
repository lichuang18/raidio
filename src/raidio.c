#include "raidio.h"
#include <stdio.h>

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
		ret = run_fio(&opt);
	} else
		ret = run_rio(&opt);

done:
	clean_rio();
	return ret;
}