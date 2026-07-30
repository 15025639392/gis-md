// stb_truetype 单一定义单元（避免多重定义链接错误）
#if __has_include(<stb_truetype.h>)
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#endif
