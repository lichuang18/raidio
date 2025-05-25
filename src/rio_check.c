#include "raidio.h"
#include <string.h>

bool is_raid(const char *file)
{
    if (!file)
        return false;
    // TODO
    // 添加判断sda是不是raid，目前只是判断是不是软raid
    // 还可添加市面其他raid方式，比如xinnor_raid
    return strstr(file, "raid") || strstr(file, "md") || strstr(file, "mdraid");
}