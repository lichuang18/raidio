#include "raidio.h"
#include <string.h>

bool is_raid(const char *file)
{
    if (!file)
        return false;

    // 简单关键字判断
    return strstr(file, "raid") || strstr(file, "md") || strstr(file, "mdraid");
}