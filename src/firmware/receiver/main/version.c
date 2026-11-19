#include "version.h"

#include "version_env.h"

/* 打印/显示的都是编译期常量，零运行时开销 */
const char *version_string(void)
{
    return SYM_VERSION_STR;
}

const char *version_build_time(void)
{
    return SYM_BUILD_TS;
}
